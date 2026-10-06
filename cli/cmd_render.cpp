#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "args.h"
#include "commands.h"
#include "edit/settings.h"
#include "imaging/geometry.h"
#include "imaging/image_io.h"
#include "imaging/output_transform.h"
#include "imaging/raw_decoder.h"
#include "imaging/renderer.h"
#include "imaging/resample.h"
#include "util/error.h"

namespace focal::cli {

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

std::string lower_ext(const std::string& path) {
    auto dot = path.find_last_of('.');
    std::string e = dot == std::string::npos ? "" : path.substr(dot + 1);
    for (auto& ch : e) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return e;
}

Settings settings_from_args(const Args& args) {
    Settings s;
    if (auto file = args.get("settings")) {
        std::ifstream in(*file);
        if (!in) throw Error(Error::Code::Io, "cannot read " + *file);
        std::stringstream ss;
        ss << in.rdbuf();
        s = settings_from_json(ss.str());
    }
    s.exposure = args.get_double("ev", s.exposure);
    s.contrast = args.get_double("contrast", s.contrast);
    s.highlights = args.get_double("highlights", s.highlights);
    s.shadows = args.get_double("shadows", s.shadows);
    s.whites = args.get_double("whites", s.whites);
    s.blacks = args.get_double("blacks", s.blacks);
    s.brightness = args.get_double("brightness", s.brightness);
    s.saturation = args.get_double("saturation", s.saturation);
    s.vibrance = args.get_double("vibrance", s.vibrance);
    s.clarity = args.get_double("clarity", s.clarity);
    s.sharpness = args.get_double("sharpness", s.sharpness);
    s.noise_reduction = args.get_double("nr", s.noise_reduction);
    s.color_noise_reduction = args.get_double("color-nr", s.color_noise_reduction);
    if (args.has("temp")) {
        s.wb.mode = WhiteBalanceSettings::Mode::Custom;
        s.wb.temperature = args.get_double("temp", 5500);
        s.wb.tint = args.get_double("tint", 0);
    }
    if (args.has("lens-correction") || args.has("lens")) s.lens.enabled = true;
    if (auto id = args.get("lens")) s.lens.id = *id;
    s.lens.distortion = args.get_double("lens-distortion", s.lens.distortion);
    s.lens.tca = args.get_double("lens-tca", s.lens.tca);
    s.lens.vignetting = args.get_double("lens-vignetting", s.lens.vignetting);
    if (auto pr = args.get("projection"); pr && !lens_projection_from_string(*pr, s.lens.projection))
        throw Error(Error::Code::InvalidArgument, "unknown --projection " + *pr);
    s.geometry.rotate90 = args.get_int("rotate", s.geometry.rotate90);
    s.geometry.straighten = args.get_double("straighten", s.geometry.straighten);
    if (auto crop = args.get("crop")) {
        CropRect c;
        if (std::sscanf(crop->c_str(), "%lf,%lf,%lf,%lf", &c.x, &c.y, &c.w, &c.h) != 4)
            throw Error(Error::Code::InvalidArgument, "--crop expects x,y,w,h");
        s.geometry.crop = c;
    }
    s.clamp();
    return s;
}

} // namespace

int cmd_render(int argc, char** argv) {
    Args args(argc, argv, {"half", "lens-correction"});
    if (args.positional().size() != 2) {
        std::fprintf(stderr, "usage: focal render <in.RAW> <out.tif|out.jpg> [options]\n");
        return 2;
    }
    const std::string in = args.positional()[0];
    const std::string out = args.positional()[1];
    const std::string ext = lower_ext(out);
    const bool jpeg = (ext == "jpg" || ext == "jpeg");
    if (!jpeg && ext != "tif" && ext != "tiff")
        throw Error(Error::Code::InvalidArgument, "output must be .tif or .jpg");

    Settings settings = settings_from_args(args);
    if (settings.lens.enabled) init_lens_db(args);

    auto t0 = Clock::now();
    const DecodedRaw raw = decode_raw(in, {.half_size = args.has("half")});
    std::fprintf(stderr, "decode   : %8.1f ms  (%d x %d)\n", ms_since(t0), raw.image.width, raw.image.height);

    // 傾き補正時はクロップ枠を自動で縮める（5.6 章）
    {
        int w2, h2;
        oriented_size(raw.image.width, raw.image.height, raw.flip, settings.geometry.rotate90, w2, h2);
        settings.geometry.crop = fit_crop_to_straighten(settings.geometry.crop, settings.geometry.straighten, w2, h2);
    }

    const int proxy_edge = args.get_int("proxy", 0);
    if (proxy_edge > 0) {
        // プレビュー経路: プロキシ → バイリニア → sRGB 8-bit
        t0 = Clock::now();
        const ImageF proxy = make_proxy(raw.image, proxy_edge);
        std::fprintf(stderr, "proxy    : %8.1f ms  (%d x %d)\n", ms_since(t0), proxy.width, proxy.height);
        const OutputTransform xf(OutputSpace::Srgb, OutputDepth::U8);
        t0 = Clock::now();
        const ImageU8 img = render_preview(raw, proxy, settings, xf);
        std::fprintf(stderr, "render   : %8.1f ms  (%d x %d)\n", ms_since(t0), img.width, img.height);
        if (jpeg)
            write_jpeg(out, img, args.get_int("quality", 92), xf.icc_profile());
        else
            write_tiff(out, img, xf.icc_profile(), TiffCompression::Deflate);
        return 0;
    }

    t0 = Clock::now();
    const ImageF linear = render_for_export(raw, settings, args.get_int("long-edge", 0));
    std::fprintf(stderr, "render   : %8.1f ms  (%d x %d)\n", ms_since(t0), linear.width, linear.height);

    t0 = Clock::now();
    if (jpeg) {
        const OutputTransform xf(OutputSpace::Srgb, OutputDepth::U8);
        write_jpeg(out, encode_u8(linear, xf), args.get_int("quality", 92), xf.icc_profile());
    } else if (args.get_int("bits", 16) == 8) {
        const OutputTransform xf(OutputSpace::Srgb, OutputDepth::U8);
        write_tiff(out, encode_u8(linear, xf), xf.icc_profile(), TiffCompression::Deflate);
    } else {
        const OutputTransform xf(OutputSpace::Srgb, OutputDepth::U16);
        write_tiff(out, encode_u16(linear, xf), xf.icc_profile());
    }
    std::fprintf(stderr, "encode   : %8.1f ms\n", ms_since(t0));
    return 0;
}

} // namespace focal::cli
