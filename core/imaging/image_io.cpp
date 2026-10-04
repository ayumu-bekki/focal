#include "imaging/image_io.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>

#include <jpeglib.h>
#include <tiffio.h>

#include "util/error.h"
#include "util/file.h"

namespace focal {

namespace {

TIFF* open_tiff(const std::filesystem::path& path, const char* mode) {
#if defined(_WIN32)
    return TIFFOpenW(path.wstring().c_str(), mode);
#else
    return TIFFOpen(path.c_str(), mode);
#endif
}

struct TiffCloser {
    void operator()(TIFF* t) const { TIFFClose(t); }
};

struct FileCloser {
    void operator()(FILE* f) const { std::fclose(f); }
};

// "YYYY-MM-DDTHH:MM:SS" → EXIF の "YYYY:MM:DD HH:MM:SS"（19 文字）。形が違えば空
std::string exif_datetime(const std::string& t) {
    if (t.size() != 19 || t[4] != '-' || t[7] != '-' || t[10] != 'T' || t[13] != ':' || t[16] != ':') return {};
    std::string s = t;
    s[4] = s[7] = ':';
    s[10] = ' ';
    return s;
}

std::string from_exif_datetime(const std::string& s) {
    if (s.size() < 19 || s[4] != ':' || s[7] != ':' || s[10] != ' ') return {};
    std::string t = s.substr(0, 19);
    t[4] = t[7] = '-';
    t[10] = 'T';
    return t;
}

// ---- EXIF（TIFF 形式、リトルエンディアン）の組み立て

struct IfdEntry {
    uint16_t tag;
    uint16_t type;  // 2 ASCII、3 SHORT、4 LONG、5 RATIONAL
    uint32_t count;
    std::vector<uint8_t> data;  // 値のバイト列（リトルエンディアン）
};

void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

IfdEntry ascii_entry(uint16_t tag, const std::string& s) {
    IfdEntry e{tag, 2, static_cast<uint32_t>(s.size() + 1), {}};
    e.data.assign(s.begin(), s.end());
    e.data.push_back(0);
    return e;
}

IfdEntry short_entry(uint16_t tag, uint16_t v) {
    IfdEntry e{tag, 3, 1, {}};
    put16(e.data, v);
    return e;
}

IfdEntry long_entry(uint16_t tag, uint32_t v) {
    IfdEntry e{tag, 4, 1, {}};
    put32(e.data, v);
    return e;
}

IfdEntry rational_entry(uint16_t tag, uint32_t num, uint32_t den) {
    IfdEntry e{tag, 5, 1, {}};
    put32(e.data, num);
    put32(e.data, den);
    return e;
}

size_t ifd_size(const std::vector<IfdEntry>& entries) {
    size_t n = 2 + 12 * entries.size() + 4;
    for (const auto& e : entries)
        if (e.data.size() > 4) n += (e.data.size() + 1) & ~size_t(1);
    return n;
}

// entries（タグの昇順）を base の位置に置く形で書く。値が 4 バイトを超えるものは IFD の後ろに並べる
void write_ifd(std::vector<uint8_t>& out, const std::vector<IfdEntry>& entries) {
    const size_t base = out.size();
    put16(out, static_cast<uint16_t>(entries.size()));
    size_t data_at = base + 2 + 12 * entries.size() + 4;
    std::vector<uint8_t> tail;
    for (const auto& e : entries) {
        put16(out, e.tag);
        put16(out, e.type);
        put32(out, e.count);
        if (e.data.size() <= 4) {
            for (size_t i = 0; i < 4; ++i) out.push_back(i < e.data.size() ? e.data[i] : 0);
        } else {
            put32(out, static_cast<uint32_t>(data_at + tail.size()));
            tail.insert(tail.end(), e.data.begin(), e.data.end());
            if (tail.size() & 1) tail.push_back(0);
        }
    }
    put32(out, 0);  // 次の IFD はない
    out.insert(out.end(), tail.begin(), tail.end());
}

// 秒・F 値・mm を RATIONAL にする（露出時間は 1/N に近い分数、そのほかは小数 1 桁）
void to_rational(double v, bool reciprocal, uint32_t& num, uint32_t& den) {
    if (reciprocal && v > 0 && v < 1) {
        num = 1;
        den = static_cast<uint32_t>(std::lround(1.0 / v));
    } else if (reciprocal) {
        num = static_cast<uint32_t>(std::lround(v * 10));
        den = 10;
    } else {
        num = static_cast<uint32_t>(std::lround(v * 10));
        den = 10;
    }
    if (den == 0) den = 1;
}

// ---- EXIF の読み取り（自分が書く項目だけ。どちらのバイト順も読む）

struct TiffReader {
    const uint8_t* p;
    size_t n;
    bool le;

    uint16_t u16(size_t at) const {
        if (at + 2 > n) return 0;
        return le ? static_cast<uint16_t>(p[at] | (p[at + 1] << 8)) : static_cast<uint16_t>((p[at] << 8) | p[at + 1]);
    }
    uint32_t u32(size_t at) const {
        if (at + 4 > n) return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[at + (le ? i : 3 - i)]) << (8 * i);
        return v;
    }
    // エントリの値の位置（4 バイト以内なら、エントリの中）
    size_t value_at(size_t entry, uint32_t bytes) const { return bytes <= 4 ? entry + 8 : u32(entry + 8); }
    std::string str(size_t entry) const {
        const uint32_t count = u32(entry + 4);
        const size_t at = value_at(entry, count);
        if (at + count > n) return {};
        std::string s(reinterpret_cast<const char*>(p + at), count);
        while (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }
    double rational(size_t entry) const {
        const size_t at = value_at(entry, 8);
        const uint32_t d = u32(at + 4);
        return d ? static_cast<double>(u32(at)) / d : 0.0;
    }
};

void read_ifd(const TiffReader& r, size_t ifd, ExifInfo& info, size_t* exif_ifd) {
    const uint16_t count = r.u16(ifd);
    for (uint16_t i = 0; i < count; ++i) {
        const size_t e = ifd + 2 + 12 * i;
        switch (r.u16(e)) {
        case 0x010F: info.make = r.str(e); break;
        case 0x0110: info.model = r.str(e); break;
        case 0x0132:
            if (info.capture_time.empty()) info.capture_time = from_exif_datetime(r.str(e));
            break;
        case 0x8769:
            if (exif_ifd) *exif_ifd = r.u32(e + 8);
            break;
        case 0x829A: info.exposure_time = r.rational(e); break;
        case 0x829D: info.f_number = r.rational(e); break;
        case 0x8827: info.iso = r.u16(e + 8); break;
        case 0x9003: info.capture_time = from_exif_datetime(r.str(e)); break;  // 撮影日時を優先する
        case 0x920A: info.focal_length = r.rational(e); break;
        case 0xA434: info.lens = r.str(e); break;
        default: break;
        }
    }
}

std::optional<ExifInfo> parse_exif_tiff(const uint8_t* p, size_t n) {
    if (n < 8) return std::nullopt;
    TiffReader r{p, n, p[0] == 'I'};
    if (!((p[0] == 'I' && p[1] == 'I') || (p[0] == 'M' && p[1] == 'M')) || r.u16(2) != 42) return std::nullopt;
    ExifInfo info;
    size_t exif_ifd = 0;
    read_ifd(r, r.u32(4), info, &exif_ifd);
    if (exif_ifd) read_ifd(r, exif_ifd, info, nullptr);
    return info;
}

void set_tiff_exif_tags(TIFF* t, const ExifInfo& e) {
    if (const auto dt = exif_datetime(e.capture_time); !dt.empty()) TIFFSetField(t, TIFFTAG_DATETIME, dt.c_str());
    if (!e.make.empty()) TIFFSetField(t, TIFFTAG_MAKE, e.make.c_str());
    if (!e.model.empty()) TIFFSetField(t, TIFFTAG_MODEL, e.model.c_str());
    TIFFSetField(t, TIFFTAG_SOFTWARE, "Focal");
}

template <class T>
void write_tiff_impl(const std::filesystem::path& path, const Image<T>& image, const std::vector<uint8_t>& icc,
                     TiffCompression compression, const ExifInfo* exif) {
    std::unique_ptr<TIFF, TiffCloser> tif(open_tiff(path, "w"));
    if (!tif) throw Error(Error::Code::Io, "cannot open for writing: " + path.string());
    TIFF* t = tif.get();
    TIFFSetField(t, TIFFTAG_IMAGEWIDTH, static_cast<uint32_t>(image.width));
    TIFFSetField(t, TIFFTAG_IMAGELENGTH, static_cast<uint32_t>(image.height));
    TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, 3);
    TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, static_cast<int>(sizeof(T) * 8));
    TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(t, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    if (compression == TiffCompression::Deflate) {
        TIFFSetField(t, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
        TIFFSetField(t, TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL);
    } else {
        TIFFSetField(t, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    }
    TIFFSetField(t, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(t, 0));
    if (!icc.empty()) TIFFSetField(t, TIFFTAG_ICCPROFILE, static_cast<uint32_t>(icc.size()), icc.data());
    if (exif) set_tiff_exif_tags(t, *exif);

    for (int y = 0; y < image.height; ++y) {
        if (TIFFWriteScanline(t, const_cast<T*>(image.row(y)), static_cast<uint32_t>(y), 0) < 0)
            throw Error(Error::Code::Io, "TIFF write failed: " + path.string());
    }
}

} // namespace

void write_tiff(const std::filesystem::path& path, const ImageU16& image, const std::vector<uint8_t>& icc,
                TiffCompression compression, const ExifInfo* exif) {
    write_tiff_impl(path, image, icc, compression, exif);
}

void write_tiff(const std::filesystem::path& path, const ImageU8& image, const std::vector<uint8_t>& icc,
                TiffCompression compression, const ExifInfo* exif) {
    write_tiff_impl(path, image, icc, compression, exif);
}

std::vector<uint8_t> build_exif_app1(const ExifInfo& info, int width, int height) {
    std::vector<IfdEntry> ifd0, exif;
    if (!info.make.empty()) ifd0.push_back(ascii_entry(0x010F, info.make));
    if (!info.model.empty()) ifd0.push_back(ascii_entry(0x0110, info.model));
    ifd0.push_back(short_entry(0x0112, 1));  // Orientation: 向きは現像で補正済み
    ifd0.push_back(ascii_entry(0x0131, "Focal"));
    const std::string dt = exif_datetime(info.capture_time);
    if (!dt.empty()) ifd0.push_back(ascii_entry(0x0132, dt));
    ifd0.push_back(long_entry(0x8769, 0));  // Exif IFD への位置（あとで入れる）

    uint32_t num = 0, den = 1;
    if (info.exposure_time && *info.exposure_time > 0) {
        to_rational(*info.exposure_time, true, num, den);
        exif.push_back(rational_entry(0x829A, num, den));
    }
    if (info.f_number && *info.f_number > 0) {
        to_rational(*info.f_number, false, num, den);
        exif.push_back(rational_entry(0x829D, num, den));
    }
    if (info.iso && *info.iso > 0 && *info.iso <= 65535) exif.push_back(short_entry(0x8827, static_cast<uint16_t>(*info.iso)));
    if (!dt.empty()) {
        exif.push_back(ascii_entry(0x9003, dt));  // DateTimeOriginal
        exif.push_back(ascii_entry(0x9004, dt));  // DateTimeDigitized
    }
    if (info.focal_length && *info.focal_length > 0) {
        to_rational(*info.focal_length, false, num, den);
        exif.push_back(rational_entry(0x920A, num, den));
    }
    exif.push_back(short_entry(0xA001, 1));  // ColorSpace: sRGB
    if (width > 0 && height > 0) {
        exif.push_back(long_entry(0xA002, static_cast<uint32_t>(width)));
        exif.push_back(long_entry(0xA003, static_cast<uint32_t>(height)));
    }
    if (!info.lens.empty()) exif.push_back(ascii_entry(0xA434, info.lens));

    // Exif IFD は IFD0 のすぐ後ろ（ヘッダ 8 バイト + IFD0 の大きさ）
    const uint32_t exif_at = static_cast<uint32_t>(8 + ifd_size(ifd0));
    for (auto& e : ifd0)
        if (e.tag == 0x8769) {
            e.data.clear();
            put32(e.data, exif_at);
        }
    std::vector<uint8_t> out = {'E', 'x', 'i', 'f', 0, 0};
    out.insert(out.end(), {'I', 'I', 42, 0});
    put32(out, 8);
    // 位置は TIFF ヘッダの先頭からの値なので、"Exif\0\0"（6 バイト）を除いた位置で組む
    std::vector<uint8_t> tiff(out.begin() + 6, out.end());
    // write_ifd は out の大きさから値の位置を決めるので、TIFF 部分だけで組んでから連結する
    write_ifd(tiff, ifd0);
    write_ifd(tiff, exif);
    out.resize(6);
    out.insert(out.end(), tiff.begin(), tiff.end());
    return out;
}

std::optional<ExifInfo> read_jpeg_exif(const std::filesystem::path& path) {
    std::unique_ptr<FILE, FileCloser> file(open_file(path, "rb"));
    if (!file) throw Error(Error::Code::Io, "cannot open: " + path.string());
    jpeg_decompress_struct cinfo{};
    jpeg_error_mgr jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jerr.error_exit = [](j_common_ptr c) {
        char msg[JMSG_LENGTH_MAX];
        (*c->err->format_message)(c, msg);
        throw Error(Error::Code::Decode, std::string("JPEG read failed: ") + msg);
    };
    jpeg_create_decompress(&cinfo);
    std::optional<ExifInfo> result;
    try {
        jpeg_stdio_src(&cinfo, file.get());
        jpeg_save_markers(&cinfo, JPEG_APP0 + 1, 0xFFFF);
        jpeg_read_header(&cinfo, TRUE);
        for (auto* m = cinfo.marker_list; m; m = m->next)
            if (m->marker == JPEG_APP0 + 1 && m->data_length > 6 && std::memcmp(m->data, "Exif\0\0", 6) == 0) {
                result = parse_exif_tiff(m->data + 6, m->data_length - 6);
                break;
            }
    } catch (...) {
        jpeg_destroy_decompress(&cinfo);
        throw;
    }
    jpeg_destroy_decompress(&cinfo);
    return result;
}

std::optional<ExifInfo> read_tiff_exif(const std::filesystem::path& path) {
    std::unique_ptr<TIFF, TiffCloser> tif(open_tiff(path, "r"));
    if (!tif) throw Error(Error::Code::Io, "cannot open: " + path.string());
    ExifInfo info;
    char* s = nullptr;
    if (TIFFGetField(tif.get(), TIFFTAG_DATETIME, &s) && s) info.capture_time = from_exif_datetime(s);
    if (TIFFGetField(tif.get(), TIFFTAG_MAKE, &s) && s) info.make = s;
    if (TIFFGetField(tif.get(), TIFFTAG_MODEL, &s) && s) info.model = s;
    if (info.empty()) return std::nullopt;
    return info;
}

void write_jpeg(const std::filesystem::path& path, const ImageU8& image, int quality,
                const std::vector<uint8_t>& icc, const ExifInfo* exif) {
    std::unique_ptr<FILE, FileCloser> file(open_file(path, "wb"));
    if (!file) throw Error(Error::Code::Io, "cannot open for writing: " + path.string());

    jpeg_compress_struct cinfo{};
    jpeg_error_mgr jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jerr.error_exit = [](j_common_ptr c) {
        char msg[JMSG_LENGTH_MAX];
        (*c->err->format_message)(c, msg);
        throw Error(Error::Code::Io, std::string("JPEG write failed: ") + msg);
    };
    jpeg_create_compress(&cinfo);
    try {
        jpeg_stdio_dest(&cinfo, file.get());
        cinfo.image_width = static_cast<JDIMENSION>(image.width);
        cinfo.image_height = static_cast<JDIMENSION>(image.height);
        cinfo.input_components = 3;
        cinfo.in_color_space = JCS_RGB;
        jpeg_set_defaults(&cinfo);
        jpeg_set_quality(&cinfo, quality, TRUE);
        jpeg_start_compress(&cinfo, TRUE);
        if (exif) {  // APP1（EXIF）は ICC（APP2）より前に置く
            const auto app1 = build_exif_app1(*exif, image.width, image.height);
            jpeg_write_marker(&cinfo, JPEG_APP0 + 1, app1.data(), static_cast<unsigned>(app1.size()));
        }
        if (!icc.empty()) jpeg_write_icc_profile(&cinfo, icc.data(), static_cast<unsigned>(icc.size()));
        while (cinfo.next_scanline < cinfo.image_height) {
            JSAMPROW row = const_cast<JSAMPROW>(image.row(static_cast<int>(cinfo.next_scanline)));
            jpeg_write_scanlines(&cinfo, &row, 1);
        }
        jpeg_finish_compress(&cinfo);
    } catch (...) {
        jpeg_destroy_compress(&cinfo);
        throw;
    }
    jpeg_destroy_compress(&cinfo);
}

LoadedImage read_tiff(const std::filesystem::path& path) {
    std::unique_ptr<TIFF, TiffCloser> tif(open_tiff(path, "r"));
    if (!tif) throw Error(Error::Code::Io, "cannot open: " + path.string());
    TIFF* t = tif.get();
    uint32_t w = 0, h = 0;
    uint16_t spp = 0, bps = 0, planar = PLANARCONFIG_CONTIG;
    TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(t, TIFFTAG_IMAGELENGTH, &h);
    TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetFieldDefaulted(t, TIFFTAG_BITSPERSAMPLE, &bps);
    TIFFGetFieldDefaulted(t, TIFFTAG_PLANARCONFIG, &planar);
    if (spp != 3 || (bps != 8 && bps != 16) || planar != PLANARCONFIG_CONTIG)
        throw Error(Error::Code::Unsupported, "unsupported TIFF layout: " + path.string());

    LoadedImage img;
    img.width = static_cast<int>(w);
    img.height = static_cast<int>(h);
    img.bits = bps;
    img.data.resize(static_cast<size_t>(w) * h * 3);
    std::vector<uint8_t> line(TIFFScanlineSize(t));
    for (uint32_t y = 0; y < h; ++y) {
        if (TIFFReadScanline(t, line.data(), y, 0) < 0) throw Error(Error::Code::Io, "TIFF read failed");
        uint16_t* dst = &img.data[static_cast<size_t>(y) * w * 3];
        if (bps == 8) {
            for (size_t i = 0; i < static_cast<size_t>(w) * 3; ++i) dst[i] = line[i];
        } else {
            std::memcpy(dst, line.data(), static_cast<size_t>(w) * 3 * 2);
        }
    }
    uint32_t icc_len = 0;
    void* icc_data = nullptr;
    if (TIFFGetField(t, TIFFTAG_ICCPROFILE, &icc_len, &icc_data) && icc_len > 0) {
        const auto* p = static_cast<const uint8_t*>(icc_data);
        img.icc.assign(p, p + icc_len);
    }
    return img;
}

ImageU8 decode_jpeg(const uint8_t* data, size_t size, int min_long_edge) {
    jpeg_decompress_struct cinfo{};
    jpeg_error_mgr jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jerr.error_exit = [](j_common_ptr c) {
        char msg[JMSG_LENGTH_MAX];
        (*c->err->format_message)(c, msg);
        throw Error(Error::Code::Decode, std::string("JPEG decode failed: ") + msg);
    };
    jerr.emit_message = [](j_common_ptr, int) {};  // 破損気味の埋め込み JPEG の警告は無視する
    jpeg_create_decompress(&cinfo);
    ImageU8 img;
    try {
        jpeg_mem_src(&cinfo, data, static_cast<unsigned long>(size));
        jpeg_read_header(&cinfo, TRUE);
        cinfo.out_color_space = JCS_RGB;
        if (min_long_edge > 0) {
            const unsigned long long_edge = std::max(cinfo.image_width, cinfo.image_height);
            int denom = 1;
            while (denom < 8 && long_edge / (denom * 2) >= static_cast<unsigned long>(min_long_edge)) denom *= 2;
            cinfo.scale_num = 1;
            cinfo.scale_denom = static_cast<unsigned>(denom);
        }
        jpeg_start_decompress(&cinfo);
        img = ImageU8(static_cast<int>(cinfo.output_width), static_cast<int>(cinfo.output_height));
        while (cinfo.output_scanline < cinfo.output_height) {
            JSAMPROW row = img.row(static_cast<int>(cinfo.output_scanline));
            jpeg_read_scanlines(&cinfo, &row, 1);
        }
        jpeg_finish_decompress(&cinfo);
    } catch (...) {
        jpeg_destroy_decompress(&cinfo);
        throw;
    }
    jpeg_destroy_decompress(&cinfo);
    return img;
}

LoadedImage read_jpeg(const std::filesystem::path& path) {
    std::unique_ptr<FILE, FileCloser> file(open_file(path, "rb"));
    if (!file) throw Error(Error::Code::Io, "cannot open: " + path.string());

    jpeg_decompress_struct cinfo{};
    jpeg_error_mgr jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jerr.error_exit = [](j_common_ptr c) {
        char msg[JMSG_LENGTH_MAX];
        (*c->err->format_message)(c, msg);
        throw Error(Error::Code::Io, std::string("JPEG read failed: ") + msg);
    };
    jpeg_create_decompress(&cinfo);
    LoadedImage img;
    try {
        jpeg_stdio_src(&cinfo, file.get());
        jpeg_save_markers(&cinfo, JPEG_APP0 + 2, 0xFFFF);
        jpeg_read_header(&cinfo, TRUE);
        cinfo.out_color_space = JCS_RGB;
        jpeg_start_decompress(&cinfo);
        img.width = static_cast<int>(cinfo.output_width);
        img.height = static_cast<int>(cinfo.output_height);
        img.bits = 8;
        img.data.resize(static_cast<size_t>(img.width) * img.height * 3);
        std::vector<uint8_t> line(static_cast<size_t>(img.width) * 3);
        while (cinfo.output_scanline < cinfo.output_height) {
            const int y = static_cast<int>(cinfo.output_scanline);
            JSAMPROW row = line.data();
            jpeg_read_scanlines(&cinfo, &row, 1);
            uint16_t* dst = &img.data[static_cast<size_t>(y) * img.width * 3];
            for (size_t i = 0; i < line.size(); ++i) dst[i] = line[i];
        }
        JOCTET* icc = nullptr;
        unsigned int icc_len = 0;
        if (jpeg_read_icc_profile(&cinfo, &icc, &icc_len) && icc) {
            img.icc.assign(icc, icc + icc_len);
            std::free(icc);
        }
        jpeg_finish_decompress(&cinfo);
    } catch (...) {
        jpeg_destroy_decompress(&cinfo);
        throw;
    }
    jpeg_destroy_decompress(&cinfo);
    return img;
}

} // namespace focal
