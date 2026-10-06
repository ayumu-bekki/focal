// 表示用の GPU レンダラー（Metal）が CPU 版（render_display）と同じ結果になること（v3.14）。
// GPU がない環境（CI の仮想マシンなど）では SKIP する
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <filesystem>
#include <random>
#include <vector>

#include "gpu/metal_renderer.h"
#include "imaging/lens_correction.h"
#include "imaging/raw_decoder.h"
#include "imaging/resample.h"

using namespace focal;

namespace {

ColorInfo srgb_camera() {
    ColorInfo c;
    c.rgb_cam = Mat3::identity();
    c.cam_xyz = rgb_to_xyz_matrix(primaries::kSrgb, primaries::kD65).inverse();
    c.as_shot_wb = {2.0, 1.0, 1.5};
    return c;
}

// なめらかな模様 + 細かい揺れ（補間の差が出やすいように）
template <class T>
Image<T> pattern(int w, int h, float maxv) {
    Image<T> img(w, h);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> u(-0.03f, 0.03f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) {
                const float v = 0.35f + 0.3f * std::sin(x * 0.013f * (c + 1) + y * 0.007f) * std::cos(y * 0.011f - c) + u(rng);
                img.data[(static_cast<size_t>(y) * w + x) * 3 + c] = static_cast<T>(std::clamp(v, 0.0f, 1.0f) * maxv);
            }
    return img;
}

struct Compare {
    int max_diff = 0;
    double mismatch = 0;  // 1 以上違う画素値の割合
};

Compare compare(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    Compare c;
    size_t n = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const int d = std::abs(a[i] - b[i]);
        c.max_diff = std::max(c.max_diff, d);
        if (d) ++n;
    }
    c.mismatch = static_cast<double>(n) / a.size();
    return c;
}

Settings edited() {
    Settings s;
    s.exposure = 0.7;
    s.contrast = 30;
    s.highlights = -40;
    s.shadows = 35;
    s.whites = 15;
    s.blacks = -10;
    s.brightness = 12;
    s.saturation = 20;
    s.vibrance = 40;
    s.wb.mode = WhiteBalanceSettings::Mode::Custom;
    s.wb.temperature = 4300;
    s.wb.tint = 12;
    s.geometry.rotate90 = 1;
    s.geometry.straighten = 3.5;
    s.geometry.crop = {0.05, 0.1, 0.8, 0.75};
    return s;
}

} // namespace

TEST_CASE("GPU: CPU 版と同じ表示用出力とヒストグラムになる", "[gpu]") {
    auto gpu = gpu::create_metal_renderer();
    if (!gpu) {
        // デバイスがあるのに作れない（シェーダーのコンパイル失敗など）なら不具合
        REQUIRE_FALSE(gpu::metal_device_available());
        SKIP("Metal が使えない");
    }
    const bool edit = GENERATE(false, true);
    const bool full = GENERATE(false, true);
    const PixelLayout layout = GENERATE(PixelLayout::Rgb8, PixelLayout::Bgrx8);

    const int sw = 1200, sh = 800;
    const ColorInfo color = srgb_camera();
    const Settings s = edit ? edited() : Settings{};
    const ColorPipeline pipeline(s, color);
    const OutputTransform display(OutputSpace::DisplayP3, OutputDepth::U8);
    const GeometryPlan plan(sw, sh, 0, s.geometry);

    GpuSource src{nullptr, nullptr, sw, sh};
    double scale = 1.0;
    PointD origin{};
    int w, h;
    if (full) {
        src.full = std::make_shared<const ImageU16>(pattern<uint16_t>(sw, sh, 65535.0f));
        origin = {37, 23};  // 100% 表示の一部
        w = 500, h = 400;
    } else {
        src.proxy = std::make_shared<const ImageF>(pattern<float>(600, 400, 1.0f));
        scale = 0.5 * 0.9;  // プロキシよりさらに小さく
        w = static_cast<int>(std::lround(plan.output_width() * scale));
        h = static_cast<int>(std::lround(plan.output_height() * scale));
    }
    const size_t bpp = layout == PixelLayout::Bgrx8 ? 4 : 3, stride = w * bpp + 8;  // 行の末尾に余り
    std::vector<uint8_t> cpu(stride * h, 0), gpu_out(stride * h, 0);
    REQUIRE(render_display(src.view(), plan, scale, origin, w, h, pipeline, display, Interpolation::Bilinear,
                           cpu.data(), stride, {}, layout));
    std::array<std::array<uint32_t, 256>, 3> hist{};
    REQUIRE(gpu->render(src, plan, scale, origin, w, h, pipeline, display, gpu_out.data(), stride, layout, hist, {}) ==
            GpuStatus::Ok);
    const Compare c = compare(cpu, gpu_out);
    CAPTURE(edit, full, c.max_diff, c.mismatch);
    CHECK(c.max_diff <= 1);
    CHECK(c.mismatch < 0.01);

    // ヒストグラムは GPU の出力と一致し、合計は画素数
    std::array<std::array<uint32_t, 256>, 3> expect{};
    const int ri = layout == PixelLayout::Bgrx8 ? 2 : 0, bi = layout == PixelLayout::Bgrx8 ? 0 : 2;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = gpu_out.data() + y * stride + x * bpp;
            ++expect[0][p[ri]];
            ++expect[1][p[1]];
            ++expect[2][p[bi]];
        }
    CHECK(hist == expect);
}

TEST_CASE("GPU: 周辺画素を使う処理（ノイズ低減・明瞭度・シャープネス）も CPU 版と同じ", "[gpu]") {
    auto gpu = gpu::create_metal_renderer();
    if (!gpu) {
        // デバイスがあるのに作れない（シェーダーのコンパイル失敗など）なら不具合
        REQUIRE_FALSE(gpu::metal_device_available());
        SKIP("Metal が使えない");
    }
    CHECK_FALSE(gpu->name().empty());
    // 1 つずつと全部。量は強め
    const int which = GENERATE(0, 1, 2, 3, 4, 5);
    const bool full = GENERATE(false, true);
    Settings s = edited();
    switch (which) {
    case 0: s.clarity = 80; break;
    case 1: s.clarity = -150; break;
    case 2: s.noise_reduction = 80; break;
    case 3: s.color_noise_reduction = 80; break;
    case 4: s.sharpness = 120; break;
    default:
        s.clarity = 60, s.noise_reduction = 60, s.color_noise_reduction = 60, s.sharpness = 100;
        break;
    }
    const int sw = 1200, sh = 800;
    const ColorPipeline pipeline(s, srgb_camera());
    REQUIRE(pipeline.needs_neighborhood());
    const OutputTransform display(OutputSpace::DisplayP3, OutputDepth::U8);
    const GeometryPlan plan(sw, sh, 0, s.geometry);
    GpuSource src{nullptr, nullptr, sw, sh};
    double scale = 1.0;
    PointD origin{};
    int w, h;
    if (full) {
        src.full = std::make_shared<const ImageU16>(pattern<uint16_t>(sw, sh, 65535.0f));
        origin = {131, 77};  // 100% 表示の一部（余白は画像の内側から取る）
        w = 300, h = 250;
    } else {
        // プロキシは縮尺 0.5（ノイズ低減・シャープネスの半径も半分）
        src.proxy = std::make_shared<const ImageF>(pattern<float>(600, 400, 1.0f));
        scale = 0.5;
        w = static_cast<int>(std::lround(plan.output_width() * scale));
        h = static_cast<int>(std::lround(plan.output_height() * scale));
    }
    const size_t stride = static_cast<size_t>(w) * 4;
    std::vector<uint8_t> cpu(stride * h), gpu_out(stride * h);
    REQUIRE(render_display(src.view(), plan, scale, origin, w, h, pipeline, display, Interpolation::Bilinear,
                           cpu.data(), stride, {}, PixelLayout::Bgrx8));
    std::array<std::array<uint32_t, 256>, 3> hist{};
    REQUIRE(gpu->render(src, plan, scale, origin, w, h, pipeline, display, gpu_out.data(), stride, PixelLayout::Bgrx8,
                        hist, {}) == GpuStatus::Ok);
    const Compare c = compare(cpu, gpu_out);
    CAPTURE(which, full, c.max_diff, c.mismatch);
    // 作業用の画像を半精度で持つので、1 違う値が数 % ある（見た目には分からない。書き出しは CPU で正確に計算する）
    CHECK(c.max_diff <= 2);
    CHECK(c.mismatch < 0.05);
    uint64_t total = 0;
    for (auto v : hist[1]) total += v;
    CHECK(total == static_cast<uint64_t>(w) * h);
}

TEST_CASE("GPU: レンズ補正（歪曲・倍率色収差・周辺減光）も CPU 版と同じ", "[gpu][lens]") {
    auto gpu = gpu::create_metal_renderer();
    if (!gpu) {
        REQUIRE_FALSE(gpu::metal_device_available());
        SKIP("Metal が使えない");
    }
    const std::filesystem::path db = FOCAL_LENSFUN_DB_DIR;
    if (!std::filesystem::is_directory(db)) SKIP("tools/fetch-lensfun-db.sh でレンズ DB を取得していない");
    set_lens_database_dirs({db});

    const bool full = GENERATE(false, true);
    const bool neighborhood = GENERATE(false, true);
    const int amount = GENERATE(100, 60);
    const int sw = 1200, sh = 800;
    RawMetadata meta;
    meta.make = "Canon";
    meta.model = "Canon EOS M50";
    meta.lens = "EF-M15-45mm f/3.5-6.3 IS STM";
    meta.focal_length = 15;
    meta.aperture = 3.5f;

    Settings s = edited();
    s.geometry.rotate90 = 0;
    s.lens.enabled = true;
    s.lens.distortion = s.lens.tca = s.lens.vignetting = amount;
    if (neighborhood) s.clarity = 50, s.sharpness = 60;
    const auto maps = build_lens_maps(s.lens, meta, sw, sh);
    REQUIRE(maps);
    REQUIRE(maps->has_tca);
    const ColorPipeline pipeline(s, srgb_camera());
    const OutputTransform display(OutputSpace::DisplayP3, OutputDepth::U8);
    const GeometryPlan plan(sw, sh, 0, s.geometry);
    GpuSource src{nullptr, nullptr, sw, sh, maps};
    double scale = 1.0;
    PointD origin{};
    int w, h;
    if (full) {
        src.full = std::make_shared<const ImageU16>(pattern<uint16_t>(sw, sh, 65535.0f));
        origin = {131, 77};
        w = 400, h = 300;
    } else {
        src.proxy = std::make_shared<const ImageF>(pattern<float>(600, 400, 1.0f));
        scale = 0.5;
        w = static_cast<int>(std::lround(plan.output_width() * scale));
        h = static_cast<int>(std::lround(plan.output_height() * scale));
    }
    const size_t stride = static_cast<size_t>(w) * 4;
    std::vector<uint8_t> cpu(stride * h), gpu_out(stride * h);
    REQUIRE(render_display(src.view(), plan, scale, origin, w, h, pipeline, display, Interpolation::Bilinear,
                           cpu.data(), stride, {}, PixelLayout::Bgrx8));
    std::array<std::array<uint32_t, 256>, 3> hist{};
    REQUIRE(gpu->render(src, plan, scale, origin, w, h, pipeline, display, gpu_out.data(), stride, PixelLayout::Bgrx8,
                        hist, {}) == GpuStatus::Ok);
    const Compare c = compare(cpu, gpu_out);
    CAPTURE(full, neighborhood, amount, c.max_diff, c.mismatch);
    CHECK(c.max_diff <= 2);
    CHECK(c.mismatch < 0.05);
}

TEST_CASE("GPU: 実際の RAW でも CPU 版と同じ（フィットと 100%）", "[gpu][data]") {
    auto gpu = gpu::create_metal_renderer();
    if (!gpu) {
        // デバイスがあるのに作れない（シェーダーのコンパイル失敗など）なら不具合
        REQUIRE_FALSE(gpu::metal_device_available());
        SKIP("Metal が使えない");
    }
    const auto path = std::filesystem::path(FOCAL_TEST_DATA_DIR) / "nikon_z7.NEF";
    if (!std::filesystem::exists(path)) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    auto raw = std::make_shared<const DecodedRaw>(decode_raw(path));
    auto proxy = std::make_shared<const ImageF>(make_proxy(raw->image, 2560));
    Settings s = edited();
    if (GENERATE(false, true)) s.clarity = 40, s.noise_reduction = 50, s.color_noise_reduction = 50, s.sharpness = 80;
    const ColorPipeline pipeline(s, raw->color);
    const OutputTransform display(OutputSpace::DisplayP3, OutputDepth::U8);
    const GeometryPlan plan(raw->image.width, raw->image.height, raw->flip, s.geometry);
    for (const bool full : {false, true}) {
        GpuSource src{nullptr, nullptr, raw->image.width, raw->image.height};
        double scale = 1;
        PointD origin{};
        int w = 1600, h = 1000;
        if (full) {
            src.full = std::shared_ptr<const ImageU16>(raw, &raw->image);
            origin = {1200, 900};
        } else {
            src.proxy = proxy;
            scale = std::min(w / plan.output_width(), h / plan.output_height());
        }
        const size_t stride = static_cast<size_t>(w) * 4;
        std::vector<uint8_t> cpu(stride * h), gpu_out(stride * h);
        REQUIRE(render_display(src.view(), plan, scale, origin, w, h, pipeline, display, Interpolation::Bilinear,
                               cpu.data(), stride, {}, PixelLayout::Bgrx8));
        std::array<std::array<uint32_t, 256>, 3> hist{};
        REQUIRE(gpu->render(src, plan, scale, origin, w, h, pipeline, display, gpu_out.data(), stride,
                            PixelLayout::Bgrx8, hist, {}) == GpuStatus::Ok);
        const Compare c = compare(cpu, gpu_out);
        CAPTURE(full, pipeline.needs_neighborhood(), c.max_diff, c.mismatch);
        CHECK(c.max_diff <= 2);
        CHECK(c.mismatch < (pipeline.needs_neighborhood() ? 0.05 : 0.01));
    }
}
