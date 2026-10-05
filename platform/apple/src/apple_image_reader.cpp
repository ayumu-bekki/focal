#include "platform/apple_image_reader.h"

#include <ImageIO/ImageIO.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "imaging/photo_file.h"
#include "util/error.h"

namespace focal::platform {

namespace {

// CF オブジェクトの持ち主（スコープを出たら解放する）
template <class T>
struct Cf {
    T ref = nullptr;
    Cf() = default;
    explicit Cf(T r) : ref(r) {}
    ~Cf() {
        if (ref) CFRelease(ref);
    }
    Cf(const Cf&) = delete;
    Cf& operator=(const Cf&) = delete;
    explicit operator bool() const { return ref != nullptr; }
};

std::string cf_string(CFTypeRef v) {
    if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return {};
    const auto s = static_cast<CFStringRef>(v);
    char buf[512];
    if (CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) return buf;
    return {};
}

double cf_number(CFTypeRef v) {
    double d = 0;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberDoubleType, &d);
    return d;
}

CFTypeRef dict_get(CFDictionaryRef d, CFStringRef key) { return d ? CFDictionaryGetValue(d, key) : nullptr; }

// "yyyy:MM:dd HH:mm:ss"（ローカル時刻）→ time_t。形が違えば 0
std::time_t parse_exif_time(const std::string& s) {
    std::tm tm{};
    if (std::sscanf(s.c_str(), "%4d:%2d:%2d %2d:%2d:%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min,
                    &tm.tm_sec) != 6)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm);
    return t < 0 ? 0 : t;
}

PlatformImage read_image(const std::filesystem::path& path, int max_long_edge) {
    const std::string utf8 = path.string();
    Cf<CFURLRef> url(CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8*>(utf8.c_str()), static_cast<CFIndex>(utf8.size()), false));
    if (!url) throw Error(Error::Code::Io, "cannot open: " + utf8);
    Cf<CGImageSourceRef> source(CGImageSourceCreateWithURL(url.ref, nullptr));
    if (!source || CGImageSourceGetCount(source.ref) < 1)
        throw Error(Error::Code::Decode, "cannot read image: " + utf8);

    PlatformImage out;
    RawMetadata& m = out.meta;
    int width = 0, height = 0, orientation = 1;
    {
        Cf<CFDictionaryRef> props(CGImageSourceCopyPropertiesAtIndex(source.ref, 0, nullptr));
        width = static_cast<int>(cf_number(dict_get(props.ref, kCGImagePropertyPixelWidth)));
        height = static_cast<int>(cf_number(dict_get(props.ref, kCGImagePropertyPixelHeight)));
        if (const double o = cf_number(dict_get(props.ref, kCGImagePropertyOrientation)); o >= 1 && o <= 8)
            orientation = static_cast<int>(o);
        const auto exif = static_cast<CFDictionaryRef>(dict_get(props.ref, kCGImagePropertyExifDictionary));
        const auto tiff = static_cast<CFDictionaryRef>(dict_get(props.ref, kCGImagePropertyTIFFDictionary));
        m.make = cf_string(dict_get(tiff, kCGImagePropertyTIFFMake));
        m.normalized_make = m.make;
        m.model = cf_string(dict_get(tiff, kCGImagePropertyTIFFModel));
        m.lens = cf_string(dict_get(exif, kCGImagePropertyExifLensModel));
        m.shutter = static_cast<float>(cf_number(dict_get(exif, kCGImagePropertyExifExposureTime)));
        m.aperture = static_cast<float>(cf_number(dict_get(exif, kCGImagePropertyExifFNumber)));
        m.focal_length = static_cast<float>(cf_number(dict_get(exif, kCGImagePropertyExifFocalLength)));
        if (const CFTypeRef iso = dict_get(exif, kCGImagePropertyExifISOSpeedRatings);
            iso && CFGetTypeID(iso) == CFArrayGetTypeID() && CFArrayGetCount(static_cast<CFArrayRef>(iso)) > 0)
            m.iso = static_cast<float>(cf_number(CFArrayGetValueAtIndex(static_cast<CFArrayRef>(iso), 0)));
        m.timestamp = parse_exif_time(cf_string(dict_get(exif, kCGImagePropertyExifDateTimeOriginal)));
    }
    if (width <= 0 || height <= 0) throw Error(Error::Code::Decode, "cannot read image size: " + utf8);
    const bool swap = orientation >= 5;
    m.width = swap ? height : width;   // 向き補正後の大きさ
    m.height = swap ? width : height;
    m.flip = 0;

    // 向きを補正した画素（長辺が max_long_edge。0 なら原寸）を、sRGB の 8-bit RGBA に描いて RGB にする
    const int target = max_long_edge > 0 ? max_long_edge : std::max(width, height);
    const void* keys[] = {kCGImageSourceCreateThumbnailFromImageAlways, kCGImageSourceCreateThumbnailWithTransform,
                          kCGImageSourceThumbnailMaxPixelSize};
    const int target_px = std::max(1, target);
    Cf<CFNumberRef> max_px(CFNumberCreate(nullptr, kCFNumberIntType, &target_px));
    const void* values[] = {kCFBooleanTrue, kCFBooleanTrue, max_px.ref};
    Cf<CFDictionaryRef> options(CFDictionaryCreate(nullptr, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks));
    Cf<CGImageRef> image(CGImageSourceCreateThumbnailAtIndex(source.ref, 0, options.ref));
    if (!image) throw Error(Error::Code::Decode, "cannot decode image: " + utf8);
    const int w = static_cast<int>(CGImageGetWidth(image.ref));
    const int h = static_cast<int>(CGImageGetHeight(image.ref));
    Cf<CGColorSpaceRef> srgb(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    Cf<CGContextRef> ctx(CGBitmapContextCreate(rgba.data(), w, h, 8, static_cast<size_t>(w) * 4, srgb.ref,
                                               kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault));
    if (!ctx) throw Error(Error::Code::Internal, "cannot create a bitmap context");
    CGContextDrawImage(ctx.ref, CGRectMake(0, 0, w, h), image.ref);
    out.pixels = ImageU8(w, h);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        out.pixels.data[i * 3 + 0] = rgba[i * 4 + 0];
        out.pixels.data[i * 3 + 1] = rgba[i * 4 + 1];
        out.pixels.data[i * 3 + 2] = rgba[i * 4 + 2];
    }
    return out;
}

} // namespace

void register_apple_image_reader() { set_platform_image_reader(&read_image); }

} // namespace focal::platform
