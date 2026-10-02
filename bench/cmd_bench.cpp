// 11.1 章の性能計測。`focal bench <in.RAW>` で実行する。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>

#include "args.h"
#include "commands.h"
#include "imaging/color_pipeline.h"
#include "imaging/geometry.h"
#include "imaging/output_transform.h"
#include "imaging/raw_decoder.h"
#include "imaging/renderer.h"
#include "imaging/resample.h"

#ifdef FOCAL_HAVE_GPU
#include "gpu/metal_renderer.h"
#endif

namespace focal::cli {

namespace {

using Clock = std::chrono::steady_clock;

struct Stats {
    double min = 0, median = 0;
};

Stats measure(int iters, const std::function<void()>& fn) {
    fn();  // ウォームアップ
    std::vector<double> t;
    for (int i = 0; i < iters; ++i) {
        const auto t0 = Clock::now();
        fn();
        t.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    }
    std::sort(t.begin(), t.end());
    return {t.front(), t[t.size() / 2]};
}

void report(const char* name, int w, int h, Stats s) {
    std::printf("%-34s %5d x %-5d  median %7.2f ms   min %7.2f ms\n", name, w, h, s.median, s.min);
}

} // namespace

int cmd_bench(int argc, char** argv) {
    Args args(argc, argv);
    if (args.positional().size() != 1) {
        std::fprintf(stderr, "usage: focal bench <in.RAW> [--proxy 2560] [--iters 30] [--clarity N] [--sharpness N] [--nr N] [--color-nr N]\n");
        return 2;
    }
    const int proxy_edge = args.get_int("proxy", 2560);
    const int iters = args.get_int("iters", 30);

    std::printf("threads: %u\n", std::thread::hardware_concurrency());
    auto t0 = Clock::now();
    const DecodedRaw raw = decode_raw(args.positional()[0]);
    std::printf("%-34s %5d x %-5d  %7.1f ms\n", "decode (LibRaw, full)", raw.image.width, raw.image.height,
                std::chrono::duration<double, std::milli>(Clock::now() - t0).count());

    ImageF proxy;
    report("proxy build (area average)", 0, 0, measure(3, [&] { proxy = make_proxy(raw.image, proxy_edge); }));
    std::printf("  proxy size: %d x %d (%.2f MP)\n", proxy.width, proxy.height, proxy.width * proxy.height / 1e6);

    Settings s;
    s.exposure = 0.5;
    s.contrast = 20;
    s.highlights = -30;
    s.shadows = 25;
    s.wb.mode = WhiteBalanceSettings::Mode::Custom;
    s.wb.temperature = 5000;
    s.clarity = args.get_double("clarity", 0.0);  // 明瞭度（周辺画素を使う処理）の計測用
    s.sharpness = args.get_double("sharpness", 0.0);
    s.noise_reduction = args.get_double("nr", 0.0);
    s.color_noise_reduction = args.get_double("color-nr", 0.0);
    const ColorPipeline pipeline(s, raw.color);
    const OutputTransform display(OutputSpace::DisplayP3, OutputDepth::U8);
    const GeometryPlan plan(raw.image.width, raw.image.height, raw.flip, s.geometry);
    const double scale = static_cast<double>(proxy.width) / raw.image.width;
    const int pw = proxy.width, ph = proxy.height;
    const SourceView psrc{nullptr, &proxy, raw.image.width, raw.image.height};

    std::vector<float> linear(static_cast<size_t>(pw) * ph * 3);
    std::vector<uint8_t> out(static_cast<size_t>(pw) * ph * 3);

    report("proxy: sample + pipeline (1)-(5)", pw, ph, measure(iters, [&] {
               render_linear(psrc, plan, scale, {}, pw, ph, pipeline, Interpolation::Bilinear, linear.data());
           }));
    report("proxy: lcms2 output (6)-(7) 1 thread", pw, ph,
           measure(std::max(3, iters / 5), [&] { display.apply(linear.data(), out.data(), static_cast<size_t>(pw) * ph); }));
    report("proxy: full display render", pw, ph, measure(iters, [&] {
               render_display(psrc, plan, scale, {}, pw, ph, pipeline, display, Interpolation::Bilinear, out.data(),
                              static_cast<size_t>(pw) * 3);
           }));

    // 2560x1440 相当（11.1 章の基準）
    {
        const int w = 2560, h = 1440;
        std::vector<uint8_t> o(static_cast<size_t>(w) * h * 3);
        report("display render 2560x1440 (proxy src)", w, h, measure(iters, [&] {
                   render_display(psrc, plan, scale, {}, w, h, pipeline, display, Interpolation::Bilinear, o.data(),
                                  static_cast<size_t>(w) * 3);
               }));
    }

    // 100% 表示: 4K 領域 + 上下左右 25% の余白（5.7 章）をフル解像度から
    {
        const int w = std::min(raw.image.width, 3840 * 3 / 2);
        const int h = std::min(raw.image.height, 2160 * 3 / 2);
        const SourceView fsrc{&raw.image, nullptr, raw.image.width, raw.image.height};
        std::vector<uint8_t> o(static_cast<size_t>(w) * h * 3);
        report("100% region (4K + 25% margin)", w, h, measure(iters, [&] {
                   render_display(fsrc, plan, 1.0, {100, 100}, w, h, pipeline, display, Interpolation::Bilinear,
                                  o.data(), static_cast<size_t>(w) * 3);
               }));
        const int w4 = std::min(raw.image.width, 3840), h4 = std::min(raw.image.height, 2160);
        report("100% region (4K only)", w4, h4, measure(iters, [&] {
                   render_display(fsrc, plan, 1.0, {100, 100}, w4, h4, pipeline, display, Interpolation::Bilinear,
                                  o.data(), static_cast<size_t>(w4) * 3);
               }));
    }

#ifdef FOCAL_HAVE_GPU
    // 表示用の GPU レンダラー（v3.14）。明瞭度などがあると CPU に任せるので測らない
    if (auto gpu = gpu::create_metal_renderer()) {
        std::printf("GPU: %s\n", gpu->name().c_str());
        std::array<std::array<uint32_t, 256>, 3> hist{};
        const auto praw = std::make_shared<const DecodedRaw>(raw);  // 計測用の複製
        const auto pproxy = std::make_shared<const ImageF>(proxy);
        auto run = [&](const char* name, const GpuSource& src, double sc, PointD origin, int w, int h) {
            std::vector<uint8_t> o(static_cast<size_t>(w) * h * 4);
            const auto status = gpu->render(src, plan, sc, origin, w, h, pipeline, display, o.data(),
                                            static_cast<size_t>(w) * 4, PixelLayout::Bgrx8, hist, {});
            if (status != GpuStatus::Ok) {
                std::printf("%-34s (GPU では描かない設定)\n", name);
                return;
            }
            report(name, w, h, measure(iters, [&] {
                       gpu->render(src, plan, sc, origin, w, h, pipeline, display, o.data(),
                                   static_cast<size_t>(w) * 4, PixelLayout::Bgrx8, hist, {});
                   }));
        };
        const GpuSource psrc_gpu{nullptr, pproxy, raw.image.width, raw.image.height};
        run("GPU display render 2560x1440", psrc_gpu, scale, {}, 2560, 1440);
        const GpuSource fsrc_gpu{std::shared_ptr<const ImageU16>(praw, &praw->image), nullptr, raw.image.width,
                                 raw.image.height};
        run("GPU 100% region (4K + 25% margin)", fsrc_gpu, 1.0, {100, 100}, std::min(raw.image.width, 3840 * 3 / 2),
            std::min(raw.image.height, 2160 * 3 / 2));
        run("GPU 100% region (4K only)", fsrc_gpu, 1.0, {100, 100}, std::min(raw.image.width, 3840),
            std::min(raw.image.height, 2160));
    }
#endif

    // 書き出し（原寸、バイキュービック）
    report("export render (full, bicubic)", raw.image.width, raw.image.height,
           measure(2, [&] { (void)render_for_export(raw, s, 0); }));
    return 0;
}

} // namespace focal::cli
