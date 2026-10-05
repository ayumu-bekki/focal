#include "imaging/raw_decoder.h"

#include "imaging/libraw_util.h"
#include "imaging/photo_file.h"

#include <algorithm>
#include <memory>

#include "util/error.h"
#include "util/file.h"

namespace focal {

namespace {

void check(int rc, const char* what, const std::filesystem::path& path) {
    if (rc == LIBRAW_SUCCESS) return;
    const auto code = (rc == LIBRAW_FILE_UNSUPPORTED) ? Error::Code::Unsupported
                      : (rc == LIBRAW_IO_ERROR)       ? Error::Code::Io
                                                      : Error::Code::Decode;
    throw Error(code, std::string(what) + " failed: " + libraw_strerror(rc) + " (" + path.string() + ")");
}

std::array<double, 3> normalize_g(const float mul[4]) {
    const double g = mul[1] > 0 ? mul[1] : 1.0;
    return {mul[0] / g, 1.0, mul[2] / g};
}

} // namespace

void open_libraw(LibRaw& raw, const std::filesystem::path& path) {
#if defined(_WIN32)
    check(raw.open_file(path.wstring().c_str()), "open_file", path);
#else
    check(raw.open_file(path.c_str()), "open_file", path);
#endif
}

RawMetadata metadata_from_libraw(const LibRaw& raw) {
    const auto& d = raw.imgdata;
    RawMetadata m;
    m.make = d.idata.make;
    m.model = d.idata.model;
    m.normalized_make = d.idata.normalized_make;
    m.lens = d.lens.Lens;
    m.iso = d.other.iso_speed;
    m.shutter = d.other.shutter;
    m.aperture = d.other.aperture;
    m.focal_length = d.other.focal_len;
    m.timestamp = d.other.timestamp;
    m.flip = d.sizes.flip;
    m.width = (m.flip & 4) ? d.sizes.height : d.sizes.width;
    m.height = (m.flip & 4) ? d.sizes.width : d.sizes.height;
    return m;
}

RawMetadata read_raw_metadata(const std::filesystem::path& path) {
    // RAW 以外の写真（JPEG・TIFF・PNG・HEIF。v3.22）は、拡張子で種類を見て別の経路で読む
    if (const auto kind = photo_kind_for_name(path_to_utf8(path.filename())); kind && *kind != PhotoKind::Raw)
        return read_image_file_metadata(path, *kind);
    auto raw = std::make_unique<LibRaw>();
    open_libraw(*raw, path);
    return metadata_from_libraw(*raw);
}

DecodedRaw decode_raw(const std::filesystem::path& path, const DecodeOptions& options) {
    if (const auto kind = photo_kind_for_name(path_to_utf8(path.filename())); kind && *kind != PhotoKind::Raw)
        return decode_image_file(path, *kind, options);
    auto raw = std::make_unique<LibRaw>();
    auto& p = raw->imgdata.params;
    p.output_color = 0;  // カメラ RGB のまま
    p.output_bps = 16;
    p.gamm[0] = p.gamm[1] = 1.0;
    p.no_auto_bright = 1;
    p.use_camera_wb = 1;
    p.highlight = 0;
    p.user_qual = 3;  // AHD
    p.half_size = options.half_size ? 1 : 0;

    if (options.cancel) {
        raw->set_progress_handler(
            [](void* data, enum LibRaw_progress, int, int) -> int {
                return static_cast<const std::atomic<bool>*>(data)->load() ? 1 : 0;
            },
            const_cast<std::atomic<bool>*>(options.cancel));
    }
    auto cancelled = [&] { return options.cancel && options.cancel->load(); };

    open_libraw(*raw, path);
    if (cancelled()) throw Error(Error::Code::Cancelled, "decode cancelled");
    check(raw->unpack(), "unpack", path);
    if (cancelled()) throw Error(Error::Code::Cancelled, "decode cancelled");

    DecodedRaw out;
    out.meta = metadata_from_libraw(*raw);
    out.flip = raw->imgdata.sizes.flip;

    // 昼光の係数は dcraw_process の前に取る（処理後は As Shot で上書きされる）
    out.color.daylight_wb = normalize_g(raw->imgdata.color.pre_mul);

    const int rc = raw->dcraw_process();
    if (cancelled() || rc == LIBRAW_CANCELLED_BY_CALLBACK) throw Error(Error::Code::Cancelled, "decode cancelled");
    check(rc, "dcraw_process", path);

    const auto& d = raw->imgdata;
    if (d.idata.colors != 3)
        throw Error(Error::Code::Unsupported, "only 3-color sensors are supported (" + path.string() + ")");

    out.color.as_shot_wb = normalize_g(d.color.pre_mul);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out.color.rgb_cam.m[r][c] = d.color.rgb_cam[r][c];

    bool has_cam_xyz = false;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            out.color.cam_xyz.m[r][c] = d.color.cam_xyz[r][c];
            has_cam_xyz |= d.color.cam_xyz[r][c] != 0.0f;
        }
    if (!has_cam_xyz) {
        // cam_xyz = diag(1/daylight) × rgb_cam⁻¹ × (XYZ→sRGB)
        const Mat3 srgb_to_xyz = rgb_to_xyz_matrix(primaries::kSrgb, primaries::kD65);
        const auto& dl = out.color.daylight_wb;
        out.color.cam_xyz =
            Mat3::diag(1.0 / dl[0], 1.0 / dl[1], 1.0 / dl[2]) * out.color.rgb_cam.inverse() * srgb_to_xyz.inverse();
    }

    // imgdata.image（4ch）から RGB 3ch をコピーし、すぐに LibRaw のメモリを解放する
    const int w = d.sizes.iwidth;
    const int h = d.sizes.iheight;
    out.image = ImageU16(w, h);
    const auto* src = d.image;
    uint16_t* dst = out.image.data.data();
    const size_t n = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < n; ++i) {
        dst[i * 3 + 0] = src[i][0];
        dst[i * 3 + 1] = src[i][1];
        dst[i * 3 + 2] = src[i][2];
    }
    raw->recycle();
    return out;
}

} // namespace focal
