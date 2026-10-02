#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include "gpu/metal_renderer.h"

#include "imaging/detail.h"
#include "imaging/local_contrast.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>
#include <string>

namespace focal::gpu {

namespace {

// shaders.metal の本文（CMake が生成）
constexpr const char* kShaderSource =
#include "shaders.metal.inc"
    ;

// shaders.metal の Params と同じ並び
struct GpuParams {
    float a, b, c, d, e, f;
    uint32_t out_w, out_h;
    uint32_t src_w, src_h;
    uint32_t src_full;
    float wb0, wb1, wb2, clip;
    float m[9];
    float tone_min_x;
    uint32_t tone_n;
    uint32_t color_adjust;
    float saturation, vibrance;
    float om[9];
    uint32_t layout;
};

// shaders.metal の Aux と同じ並び
struct AuxParams {
    int32_t w = 0, h = 0;
    int32_t r = 0;
    int32_t f = 1, gx0 = 0, gy0 = 0, sw = 0, sh = 0;
    int32_t ox = 0, oy = 0;
    float amount = 0, eps = 0, th2 = 0;
    int32_t left = 0, top = 0, bw = 0;
};

// metal-cpp のオブジェクトを解放する
template <class T>
struct Released {
    void operator()(T* p) const {
        if (p) p->release();
    }
};
template <class T>
using Ref = std::unique_ptr<T, Released<T>>;

// 自動解放される一時オブジェクト（コマンドバッファなど）のため、呼び出しごとにプールを置く
struct Pool {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    ~Pool() { pool->release(); }
};

class MetalRenderer final : public GpuRenderer {
public:
    static std::unique_ptr<MetalRenderer> create() {
        const Pool pool;
        Ref<MTL::Device> device(MTL::CreateSystemDefaultDevice());
        if (!device) return nullptr;
        auto r = std::unique_ptr<MetalRenderer>(new MetalRenderer(std::move(device)));
        return r->init() ? std::move(r) : nullptr;
    }

    std::string name() const override { return device_->name()->utf8String(); }

    GpuStatus render(const GpuSource& src, const GeometryPlan& plan, double scale, PointD origin, int w, int h,
                     const ColorPipeline& pipeline, const OutputTransform& transform, uint8_t* out, size_t stride,
                     PixelLayout layout, std::array<std::array<uint32_t, 256>, 3>& histogram,
                     const CancelToken& cancel) override {
        if (transform.depth() != OutputDepth::U8 || w <= 0 || h <= 0) return GpuStatus::Unsupported;
        const Pool pool;
        std::lock_guard lock(mutex_);
        if (cancel.cancelled()) return GpuStatus::Cancelled;
        MTL::Buffer* source = upload_source(src);
        if (!source) return GpuStatus::Failed;

        const SourceView view = src.view();
        // 原寸より大きく描く（縮尺 > 1）と、ノイズ低減・シャープネスの窓がタイルの余白に収まらない。その場合は CPU で描く
        if (pipeline.needs_neighborhood() &&
            (detail::luma_radius(scale) > kMaxHalo ||
             static_cast<int>(detail::sharpen_kernel(pipeline.detail(), scale).size() / 2) > kMaxHalo))
            return GpuStatus::Unsupported;
        // 周辺画素を使う処理（5a）があれば、余白込みの範囲を描く（CPU 版の render_neighborhood と同じ）
        const bool neighborhood = pipeline.needs_neighborhood();
        const NeighborhoodRegion region = neighborhood ? plan_neighborhood(view, plan, scale, origin, w, h, pipeline)
                                                       : NeighborhoodRegion{};
        const Affine m = output_to_source(view, plan, scale,
                                          neighborhood ? PointD{origin.x - region.left, origin.y - region.top} : origin);
        GpuParams p{};
        p.a = static_cast<float>(m.a), p.b = static_cast<float>(m.b), p.c = static_cast<float>(m.c);
        p.d = static_cast<float>(m.d), p.e = static_cast<float>(m.e), p.f = static_cast<float>(m.f);
        p.out_w = static_cast<uint32_t>(w), p.out_h = static_cast<uint32_t>(h);
        p.src_w = static_cast<uint32_t>(view.width()), p.src_h = static_cast<uint32_t>(view.height());
        p.src_full = src.full ? 1 : 0;
        p.wb0 = pipeline.wb_ratio()[0], p.wb1 = pipeline.wb_ratio()[1], p.wb2 = pipeline.wb_ratio()[2];
        p.clip = pipeline.wb_clip();
        std::copy_n(pipeline.matrix(), 9, p.m);
        p.tone_min_x = pipeline.tone().min_x();
        p.tone_n = static_cast<uint32_t>(pipeline.tone().table().size());
        p.color_adjust = pipeline.color_adjust() ? 1 : 0;
        p.saturation = pipeline.saturation(), p.vibrance = pipeline.vibrance();
        std::copy_n(transform.fast_matrix(), 9, p.om);
        p.layout = layout == PixelLayout::Bgrx8 ? 1 : 0;

        // 小さな表（トーン 16KB、TRC 64KB）は毎回書く。出力とヒストグラムの置き場は使い回す
        const auto& lut = pipeline.tone().table();
        const auto& trc = transform.fast_trc();
        ensure(tone_buf_, lut.size() * sizeof(float));
        ensure(trc_buf_, trc.size());
        std::memcpy(tone_buf_->contents(), lut.data(), lut.size() * sizeof(float));
        std::memcpy(trc_buf_->contents(), trc.data(), trc.size());
        const size_t bpp = layout == PixelLayout::Bgrx8 ? 4 : 3;
        const size_t row = static_cast<size_t>(w) * bpp;
        ensure(out_buf_, row * static_cast<size_t>(h));
        ensure(hist_buf_, 768 * sizeof(uint32_t));
        std::memset(hist_buf_->contents(), 0, 768 * sizeof(uint32_t));

        MTL::CommandBuffer* cmd = queue_->commandBuffer();
        MTL::ComputeCommandEncoder* enc = cmd->computeCommandEncoder();
        if (!neighborhood) {
            enc->setComputePipelineState(kernel("render_base"));
            enc->setBytes(&p, sizeof(p), 0);
            enc->setBuffer(src.full ? source : dummy_.get(), 0, 1);
            enc->setBuffer(src.full ? dummy_.get() : source, 0, 2);
            enc->setBuffer(tone_buf_.get(), 0, 3);
            enc->setBuffer(trc_buf_.get(), 0, 4);
            enc->setBuffer(out_buf_.get(), 0, 5);
            enc->setBuffer(hist_buf_.get(), 0, 6);
            dispatch(enc, w, h);
        } else {
            encode_neighborhood(enc, p, source, src.full != nullptr, region, scale, pipeline, w, h);
        }
        enc->endEncoding();
        const auto t0 = std::chrono::steady_clock::now();
        cmd->commit();
        cmd->waitUntilCompleted();
        if (std::getenv("FOCAL_GPU_TIMING")) {
            const auto t1 = std::chrono::steady_clock::now();
            std::fprintf(stderr, "gpu %.2f ms, wait %.2f ms\n", (cmd->GPUEndTime() - cmd->GPUStartTime()) * 1000,
                         std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        if (cmd->status() != MTL::CommandBufferStatusCompleted) return GpuStatus::Failed;
        if (cancel.cancelled()) return GpuStatus::Cancelled;

        const auto* o = static_cast<const uint8_t*>(out_buf_->contents());
        for (int y = 0; y < h; ++y) std::memcpy(out + static_cast<size_t>(y) * stride, o + static_cast<size_t>(y) * row, row);
        const auto* hs = static_cast<const uint32_t*>(hist_buf_->contents());
        for (int c = 0; c < 3; ++c) std::copy_n(hs + c * 256, 256, histogram[static_cast<size_t>(c)].begin());
        return GpuStatus::Ok;
    }

    void release_sources() override {
        std::lock_guard lock(mutex_);
        full_ = {};
        proxy_ = {};
    }

private:
    explicit MetalRenderer(Ref<MTL::Device> device) : device_(std::move(device)) {}

    static constexpr const char* kKernels[] = {"render_base", "render_tone", "box_h",      "box_v",
                                                "sqrt_lum1",   "nr_tile1",    "nr_tile2",   "cnr_down",
                                                "cnr_coef",    "cnr_apply",   "lc_down_wk", "lc_coef",
                                                "lc_apply_wk", "sh_tile",     "finish"};

    MTL::ComputePipelineState* kernel(const char* name) { return pipelines_.at(name).get(); }

    static constexpr int kMaxHalo = 8;  // shaders.metal のタイルの余白の最大

    static void dispatch(MTL::ComputeCommandEncoder* enc, int w, int h) {
        enc->dispatchThreads(MTL::Size(static_cast<NS::UInteger>(w), static_cast<NS::UInteger>(h), 1),
                             MTL::Size(16, 16, 1));
    }

    // 補助のカーネル（Aux を buffer(0) に、平面を順に buffer(1..) に）
    void aux(MTL::ComputeCommandEncoder* enc, const char* name, const AuxParams& a, int gw, int gh,
             std::initializer_list<MTL::Buffer*> bufs) {
        enc->setComputePipelineState(kernel(name));
        enc->setBytes(&a, sizeof(a), 0);
        NS::UInteger i = 1;
        for (MTL::Buffer* b : bufs) enc->setBuffer(b, 0, i++);
        dispatch(enc, gw, gh);
    }

    // 箱型の平均（横 → 縦）。src → dst、tmp は作業用
    void box(MTL::ComputeCommandEncoder* enc, AuxParams a, int r, MTL::Buffer* src, MTL::Buffer* dst, MTL::Buffer* tmp) {
        a.r = r;
        aux(enc, "box_h", a, a.w, a.h, {src, tmp});
        aux(enc, "box_v", a, a.w, a.h, {tmp, dst});
    }

    MTL::Buffer* plane(size_t slot, size_t floats) {
        if (planes_.size() <= slot) planes_.resize(slot + 1);
        ensure(planes_[slot], floats * sizeof(float));
        return planes_[slot].get();
    }

    // 5.4 章 (5a): ノイズ低減（輝度 → カラー）→ 明瞭度 → シャープネス → 仕上げ。CPU 版と同じ順番・同じ条件
    void encode_neighborhood(MTL::ComputeCommandEncoder* enc, GpuParams p, MTL::Buffer* source, bool full,
                             const NeighborhoodRegion& region, double scale, const ColorPipeline& pipeline, int w, int h) {
        const int bw = region.bw, bh = region.bh;
        const size_t n = static_cast<size_t>(bw) * bh;
        MTL::Buffer* wk = plane(0, (n * 3 + 1) / 2);  // half × 3（plane は float の個数で確保する）
        GpuParams tp = p;
        tp.out_w = static_cast<uint32_t>(bw), tp.out_h = static_cast<uint32_t>(bh);
        enc->setComputePipelineState(kernel("render_tone"));
        enc->setBytes(&tp, sizeof(tp), 0);
        enc->setBuffer(full ? source : dummy_.get(), 0, 1);
        enc->setBuffer(full ? dummy_.get() : source, 0, 2);
        enc->setBuffer(tone_buf_.get(), 0, 3);
        enc->setBuffer(wk, 0, 4);
        dispatch(enc, bw, bh);

        AuxParams base;
        base.w = bw, base.h = bh;
        base.ox = region.ox - region.left, base.oy = region.oy - region.top;
        const DetailParams& dp = pipeline.detail();

        // 輝度のノイズ低減
        // （タイルで 2 回にまとめる。窓の半径はタイルの余白 kMaxHalo 以下であること。render() で確かめる）
        if (const int r = detail::luma_radius(scale); dp.noise_reduction > 0 && r >= 1) {
            AuxParams a = base;
            a.r = r;
            a.eps = (detail::kLumaEpsMax * dp.noise_reduction) * (detail::kLumaEpsMax * dp.noise_reduction);
            MTL::Buffer *s = plane(1, n), *ca = plane(2, n), *cb = plane(3, n);
            aux(enc, "sqrt_lum1", a, bw, bh, {wk, s});
            aux(enc, "nr_tile1", a, bw, bh, {s, ca, cb});
            aux(enc, "nr_tile2", a, bw, bh, {wk, s, ca, cb});
        }

        // カラーのノイズ低減（縮小して係数を計算する）
        if (const int rc = detail::chroma_radius(dp, scale); dp.color_noise_reduction > 0 && rc >= 1) {
            const int f = detail::chroma_factor(rc), rs = std::max(1, rc / f);
            AuxParams a = base;
            auto floor_div = [](int v, int d) { return v >= 0 ? v / d : -((-v + d - 1) / d); };
            a.f = f;
            a.gx0 = floor_div(a.ox, f), a.gy0 = floor_div(a.oy, f);
            a.sw = floor_div(a.ox + bw - 1, f) - a.gx0 + 1, a.sh = floor_div(a.oy + bh - 1, f) - a.gy0 + 1;
            const size_t sn = static_cast<size_t>(a.sw) * a.sh;
            MTL::Buffer *lg = plane(7, sn), *lgg = plane(8, sn), *lc0 = plane(9, sn), *lc1 = plane(10, sn),
                        *lg0 = plane(11, sn), *lg1 = plane(12, sn), *mg = plane(13, sn), *mgg = plane(14, sn),
                        *m0 = plane(15, sn), *m1 = plane(16, sn), *mg0 = plane(17, sn), *mg1 = plane(18, sn),
                        *tmp = plane(19, sn);
            aux(enc, "cnr_down", a, a.sw, a.sh, {wk, lg, lgg, lc0, lc1, lg0, lg1});
            AuxParams s = a;
            s.w = a.sw, s.h = a.sh;
            box(enc, s, rs, lg, mg, tmp);
            box(enc, s, rs, lgg, mgg, tmp);
            box(enc, s, rs, lc0, m0, tmp);
            box(enc, s, rs, lc1, m1, tmp);
            box(enc, s, rs, lg0, mg0, tmp);
            box(enc, s, rs, lg1, mg1, tmp);
            s.eps = detail::kChromaEps;
            aux(enc, "cnr_coef", s, a.sw, a.sh, {mg, mgg, m0, m1, mg0, mg1});
            box(enc, s, rs, m0, lc0, tmp);
            box(enc, s, rs, m1, lc1, tmp);
            box(enc, s, rs, mg0, lg0, tmp);
            box(enc, s, rs, mg1, lg1, tmp);
            aux(enc, "cnr_apply", a, bw, bh, {wk, lc0, lg0, lc1, lg1});
        }

        // 明瞭度
        if (pipeline.clarity() != 0.0f && region.sigma >= 0.5) {
            const LocalContrastPlan lp = plan_local_contrast(region.sigma, base.ox, base.oy, bw, bh);
            AuxParams a = base;
            a.f = lp.f, a.gx0 = lp.gx0, a.gy0 = lp.gy0, a.sw = lp.sw, a.sh = lp.sh;
            a.amount = pipeline.clarity();
            const size_t sn = static_cast<size_t>(lp.sw) * lp.sh;
            MTL::Buffer *ca = plane(7, sn), *ii = plane(8, sn), *cb = plane(9, sn), *tmp = plane(10, sn);
            aux(enc, "lc_down_wk", a, lp.sw, lp.sh, {wk, ca, ii});
            AuxParams s = a;
            s.w = lp.sw, s.h = lp.sh;
            // 箱型 3 回（ガウスぼかしの近似、local_contrast.cpp の gaussian_approx）。r が 0 なら行わない
            auto gauss = [&](MTL::Buffer* buf) {
                if (lp.r < 1) return;
                for (int pass = 0; pass < 3; ++pass) box(enc, s, lp.r, buf, buf, tmp);
            };
            gauss(ca);  // 平均
            gauss(ii);  // 二乗の平均
            aux(enc, "lc_coef", s, lp.sw, lp.sh, {ca, ii, cb});
            gauss(ca);
            gauss(cb);
            aux(enc, "lc_apply_wk", a, bw, bh, {wk, ca, cb});
        }

        // シャープネス
        if (const std::vector<float> k = detail::sharpen_kernel(dp, scale); !k.empty()) {
            AuxParams a = base;
            a.r = static_cast<int32_t>(k.size() / 2);
            a.amount = dp.sharpness;
            a.th2 = detail::kSharpenThreshold * detail::kSharpenThreshold;
            ensure(kernel_buf_, k.size() * sizeof(float));
            std::memcpy(kernel_buf_->contents(), k.data(), k.size() * sizeof(float));
            MTL::Buffer* s = plane(1, n);
            aux(enc, "sqrt_lum1", a, bw, bh, {wk, s});
            enc->setComputePipelineState(kernel("sh_tile"));
            enc->setBytes(&a, sizeof(a), 0);
            enc->setBuffer(wk, 0, 1);
            enc->setBuffer(s, 0, 2);
            enc->setBuffer(kernel_buf_.get(), 0, 3);
            dispatch(enc, bw, bh);
        }

        finish(enc, p, base, region, wk, w, h);
    }

    // 仕上げ: 余白込みのバッファから描く範囲を取り出して (5b)〜(7)
    void finish(MTL::ComputeCommandEncoder* enc, const GpuParams& p, const AuxParams& base,
                const NeighborhoodRegion& region, MTL::Buffer* wk, int w, int h) {
        AuxParams fin = base;
        fin.left = region.left, fin.top = region.top, fin.bw = region.bw;
        enc->setComputePipelineState(kernel("finish"));
        enc->setBytes(&p, sizeof(p), 0);
        enc->setBytes(&fin, sizeof(fin), 1);
        enc->setBuffer(wk, 0, 2);
        enc->setBuffer(trc_buf_.get(), 0, 3);
        enc->setBuffer(out_buf_.get(), 0, 4);
        enc->setBuffer(hist_buf_.get(), 0, 5);
        dispatch(enc, w, h);
    }

    bool init() {
        queue_.reset(device_->newCommandQueue());
        if (!queue_) return false;
        NS::Error* error = nullptr;
        Ref<MTL::CompileOptions> options(MTL::CompileOptions::alloc()->init());
        // CPU 版と同じ結果にするため、近似の数学関数を使わない
        options->setMathMode(MTL::MathModeSafe);
        Ref<MTL::Library> library(device_->newLibrary(NS::String::string(kShaderSource, NS::UTF8StringEncoding),
                                                      options.get(), &error));
        if (!library) {
            std::fprintf(stderr, "focal: Metal shader compile failed: %s\n",
                         error ? error->localizedDescription()->utf8String() : "?");
            return false;
        }
        for (const char* name : kKernels) {
            Ref<MTL::Function> fn(library->newFunction(NS::String::string(name, NS::UTF8StringEncoding)));
            Ref<MTL::ComputePipelineState> ps(fn ? device_->newComputePipelineState(fn.get(), &error) : nullptr);
            if (!ps) {
                std::fprintf(stderr, "focal: Metal kernel '%s' is not available\n", name);
                return false;
            }
            pipelines_.emplace(name, std::move(ps));
        }
        dummy_.reset(device_->newBuffer(16, MTL::ResourceStorageModeShared));
        return dummy_ != nullptr;
    }

    void ensure(Ref<MTL::Buffer>& buf, size_t bytes) {
        if (!buf || buf->length() < bytes) buf.reset(device_->newBuffer(bytes, MTL::ResourceStorageModeShared));
    }

    // 描画元を GPU から見える置き場に写す。同じ画像（weak_ptr で確かめる）なら使い回す。
    // フル解像度（100% 表示）とプロキシ（フィット表示）は交互に使うので、別々に取っておく
    struct Cached {
        std::weak_ptr<const void> owner;
        Ref<MTL::Buffer> buf;
    };
    MTL::Buffer* upload_source(const GpuSource& src) {
        std::shared_ptr<const void> owner = src.full ? std::shared_ptr<const void>(src.full)
                                                     : std::shared_ptr<const void>(src.proxy);
        if (!owner) return nullptr;
        Cached& c = src.full ? full_ : proxy_;
        if (const auto cached = c.owner.lock(); cached && cached == owner && c.buf) return c.buf.get();
        const void* data = src.full ? static_cast<const void*>(src.full->data.data())
                                    : static_cast<const void*>(src.proxy->data.data());
        const size_t bytes = src.full ? src.full->data.size() * sizeof(uint16_t) : src.proxy->data.size() * sizeof(float);
        c.buf.reset();  // 先に古いものを解放する（大きいので 2 つ同時に持たない）
        c.buf.reset(device_->newBuffer(data, bytes, MTL::ResourceStorageModeShared));
        c.owner = owner;
        return c.buf.get();
    }

    Ref<MTL::Device> device_;
    Ref<MTL::CommandQueue> queue_;
    std::map<std::string, Ref<MTL::ComputePipelineState>> pipelines_;
    Ref<MTL::Buffer> dummy_, tone_buf_, trc_buf_, out_buf_, hist_buf_, kernel_buf_;
    Cached full_, proxy_;  // 描画元（フル解像度、プロキシ）
    std::vector<Ref<MTL::Buffer>> planes_;  // 作業用の平面（使い回す）
    std::mutex mutex_;
};

} // namespace

bool metal_device_available() {
    const Pool pool;
    Ref<MTL::Device> device(MTL::CreateSystemDefaultDevice());
    return device != nullptr;
}

std::unique_ptr<GpuRenderer> create_metal_renderer() {
    if (const char* e = std::getenv("FOCAL_GPU"); e && std::string(e) == "0") return nullptr;
    return MetalRenderer::create();
}

} // namespace focal::gpu
