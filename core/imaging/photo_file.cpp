#include "imaging/photo_file.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <ctime>
#include <mutex>
#include <set>
#include <string>

#include "imaging/image_io.h"
#include "thumbs/thumbnail.h"
#include "util/error.h"
#include "util/file.h"
#include "util/matrix.h"

namespace focal {

namespace fs = std::filesystem;

namespace {

const std::set<std::string>& raw_extensions() {
    static const std::set<std::string> exts = {
        "3fr", "arw", "cr2", "cr3", "crw", "dcr", "dng", "erf", "fff", "gpr", "iiq", "kdc", "mef", "mos",
        "mrw", "nef", "nrw", "orf", "pef", "raf", "raw", "rw2", "rwl", "sr2", "srf", "srw", "x3f",
    };
    return exts;
}

std::mutex g_reader_mutex;
PlatformImageReader g_reader;

PlatformImageReader platform_reader() {
    std::lock_guard lock(g_reader_mutex);
    return g_reader;
}

// "YYYY-MM-DDTHH:MM:SS"（ローカル時刻）→ time_t。形が違えば 0
std::time_t parse_local_time(const std::string& s) {
    if (s.size() != 19) return 0;
    std::tm tm{};
    if (std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
                    &tm.tm_min, &tm.tm_sec) != 6)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm);
    return t < 0 ? 0 : t;
}

RawMetadata metadata_from(const ImageHeader& h) {
    RawMetadata m;
    const int flip = h.exif && h.exif->orientation ? flip_from_exif_orientation(*h.exif->orientation) : 0;
    m.flip = flip;
    m.width = (flip & 4) ? h.height : h.width;
    m.height = (flip & 4) ? h.width : h.height;
    if (h.exif) {
        const ExifInfo& e = *h.exif;
        m.make = e.make;
        m.normalized_make = e.make;
        m.model = e.model;
        m.lens = e.lens;
        m.iso = e.iso ? static_cast<float>(*e.iso) : 0.0f;
        m.shutter = e.exposure_time ? static_cast<float>(*e.exposure_time) : 0.0f;
        m.aperture = e.f_number ? static_cast<float>(*e.f_number) : 0.0f;
        m.focal_length = e.focal_length ? static_cast<float>(*e.focal_length) : 0.0f;
        m.timestamp = parse_local_time(e.capture_time);
    }
    return m;
}

ImageHeader read_header(const fs::path& path, PhotoKind kind) {
    switch (kind) {
    case PhotoKind::Jpeg: return read_jpeg_header(path);
    case PhotoKind::Tiff: return read_tiff_header(path);
    case PhotoKind::Png: return read_png_header(path);
    default: throw Error(Error::Code::Unsupported, "not a standard image file: " + path_to_utf8(path));
    }
}

// 画素（向き補正前）と、その ICC を読む。sRGB に変換済みで返す
ImageU8 read_pixels(const fs::path& path, PhotoKind kind, int min_long_edge, const ImageHeader& header) {
    ImageU8 img;
    switch (kind) {
    case PhotoKind::Jpeg: img = decode_jpeg_file(path, min_long_edge); break;
    case PhotoKind::Tiff: img = decode_tiff_file(path); break;
    case PhotoKind::Png: img = decode_png_file(path); break;
    default: throw Error(Error::Code::Unsupported, "not a standard image file: " + path_to_utf8(path));
    }
    convert_to_srgb(img, header.icc);
    return img;
}

// sRGB の 8-bit 値 → リニア 16-bit
const std::array<uint16_t, 256>& srgb_to_linear16() {
    static const std::array<uint16_t, 256> lut = [] {
        std::array<uint16_t, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const double c = i / 255.0;
            const double lin = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
            t[static_cast<size_t>(i)] = static_cast<uint16_t>(std::lround(lin * 65535.0));
        }
        return t;
    }();
    return lut;
}

DecodedRaw to_decoded(const ImageU8& img, const RawMetadata& meta, int flip) {
    DecodedRaw out;
    out.meta = meta;
    out.flip = flip;
    out.image = ImageU16(img.width, img.height);
    const auto& lut = srgb_to_linear16();
    const size_t n = img.data.size();
    for (size_t i = 0; i < n; ++i) out.image.data[i] = lut[img.data[i]];
    // カメラ RGB = リニア sRGB。As Shot = 昼光 = (1,1,1)
    out.color.as_shot_wb = {1, 1, 1};
    out.color.daylight_wb = {1, 1, 1};
    out.color.rgb_cam = Mat3::identity();
    out.color.cam_xyz = rgb_to_xyz_matrix(primaries::kSrgb, primaries::kD65).inverse();
    out.color.display_referred = true;  // すでに表示用の階調なので、ベースカーブは入れない
    return out;
}

} // namespace

std::optional<PhotoKind> photo_kind_for_name(std::string_view name) {
    const auto dot = name.find_last_of('.');
    if (dot == std::string_view::npos) return std::nullopt;
    std::string ext(name.substr(dot + 1));
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (raw_extensions().count(ext)) return PhotoKind::Raw;
    if (ext == "jpg" || ext == "jpeg") return PhotoKind::Jpeg;
    if (ext == "tif" || ext == "tiff") return PhotoKind::Tiff;
    if (ext == "png") return PhotoKind::Png;
    if ((ext == "heic" || ext == "heif") && platform_image_reader_available()) return PhotoKind::Heif;
    return std::nullopt;
}

void set_platform_image_reader(PlatformImageReader reader) {
    std::lock_guard lock(g_reader_mutex);
    g_reader = std::move(reader);
}

bool platform_image_reader_available() {
    std::lock_guard lock(g_reader_mutex);
    return static_cast<bool>(g_reader);
}

RawMetadata read_image_file_metadata(const fs::path& path, PhotoKind kind) {
    if (kind == PhotoKind::Heif) {
        const auto reader = platform_reader();
        if (!reader) throw Error(Error::Code::Unsupported, "HEIF is not supported on this OS");
        return reader(path, 64).meta;  // 小さく読めば速い。メタデータと大きさ（原寸）は meta に入る
    }
    return metadata_from(read_header(path, kind));
}

DecodedRaw decode_image_file(const fs::path& path, PhotoKind kind, const DecodeOptions& options) {
    auto cancelled = [&] { return options.cancel && options.cancel->load(); };
    if (kind == PhotoKind::Heif) {
        const auto reader = platform_reader();
        if (!reader) throw Error(Error::Code::Unsupported, "HEIF is not supported on this OS");
        PlatformImage img = reader(path, 0);
        if (cancelled()) throw Error(Error::Code::Cancelled, "decode cancelled");
        img.meta.flip = 0;
        img.meta.width = img.pixels.width;
        img.meta.height = img.pixels.height;
        return to_decoded(img.pixels, img.meta, 0);
    }
    const ImageHeader header = read_header(path, kind);
    if (cancelled()) throw Error(Error::Code::Cancelled, "decode cancelled");
    const RawMetadata meta = metadata_from(header);
    const ImageU8 pixels = read_pixels(path, kind, 0, header);
    if (cancelled()) throw Error(Error::Code::Cancelled, "decode cancelled");
    return to_decoded(pixels, meta, meta.flip);
}

ImageU8 image_file_thumbnail(const fs::path& path, PhotoKind kind, int long_edge) {
    if (kind == PhotoKind::Heif) {
        const auto reader = platform_reader();
        if (!reader) throw Error(Error::Code::Unsupported, "HEIF is not supported on this OS");
        return reader(path, long_edge).pixels;
    }
    const ImageHeader header = read_header(path, kind);
    const int flip = header.exif && header.exif->orientation ? flip_from_exif_orientation(*header.exif->orientation) : 0;
    const ImageU8 pixels = read_pixels(path, kind, long_edge, header);
    return apply_orientation(downscale_u8(pixels, long_edge), flip);
}

} // namespace focal
