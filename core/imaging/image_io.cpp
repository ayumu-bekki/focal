#include "imaging/image_io.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
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

template <class T>
void write_tiff_impl(const std::filesystem::path& path, const Image<T>& image, const std::vector<uint8_t>& icc,
                     TiffCompression compression) {
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

    for (int y = 0; y < image.height; ++y) {
        if (TIFFWriteScanline(t, const_cast<T*>(image.row(y)), static_cast<uint32_t>(y), 0) < 0)
            throw Error(Error::Code::Io, "TIFF write failed: " + path.string());
    }
}

} // namespace

void write_tiff(const std::filesystem::path& path, const ImageU16& image, const std::vector<uint8_t>& icc,
                TiffCompression compression) {
    write_tiff_impl(path, image, icc, compression);
}

void write_tiff(const std::filesystem::path& path, const ImageU8& image, const std::vector<uint8_t>& icc,
                TiffCompression compression) {
    write_tiff_impl(path, image, icc, compression);
}

void write_jpeg(const std::filesystem::path& path, const ImageU8& image, int quality,
                const std::vector<uint8_t>& icc) {
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
