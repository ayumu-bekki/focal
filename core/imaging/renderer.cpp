#include "imaging/renderer.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

#include "imaging/detail.h"
#include "imaging/local_contrast.h"
#include "imaging/resample.h"
#include "util/error.h"

namespace focal {

namespace {

constexpr int kRowBlock = 16;
constexpr float kU16ToFloat = 1.0f / 65535.0f;

template <class T>
float to_float(T v);
template <>
inline float to_float<uint16_t>(uint16_t v) { return v * kU16ToFloat; }
template <>
inline float to_float<float>(float v) { return v; }

// x, y は画素中心を 0.5 とする連続座標
template <class T>
inline void sample_bilinear(const Image<T>& img, double x, double y, float* out) {
    const double fx = x - 0.5, fy = y - 0.5;
    int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    const float tx = static_cast<float>(fx - x0), ty = static_cast<float>(fy - y0);
    const int x1 = std::clamp(x0 + 1, 0, img.width - 1), y1 = std::clamp(y0 + 1, 0, img.height - 1);
    x0 = std::clamp(x0, 0, img.width - 1);
    y0 = std::clamp(y0, 0, img.height - 1);
    const T* r0 = img.row(y0);
    const T* r1 = img.row(y1);
    for (int c = 0; c < 3; ++c) {
        const float a = to_float(r0[x0 * 3 + c]), b = to_float(r0[x1 * 3 + c]);
        const float d = to_float(r1[x0 * 3 + c]), e = to_float(r1[x1 * 3 + c]);
        const float top = a + (b - a) * tx, bot = d + (e - d) * tx;
        out[c] = top + (bot - top) * ty;
    }
}

// Catmull-Rom（Keys, a = -0.5）
inline void cubic_weights(float t, float w[4]) {
    const float t2 = t * t, t3 = t2 * t;
    w[0] = -0.5f * t3 + t2 - 0.5f * t;
    w[1] = 1.5f * t3 - 2.5f * t2 + 1.0f;
    w[2] = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
    w[3] = 0.5f * t3 - 0.5f * t2;
}

template <class T>
inline void sample_bicubic(const Image<T>& img, double x, double y, float* out) {
    const double fx = x - 0.5, fy = y - 0.5;
    const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
    float wx[4], wy[4];
    cubic_weights(static_cast<float>(fx - ix), wx);
    cubic_weights(static_cast<float>(fy - iy), wy);
    float acc[3] = {0, 0, 0};
    for (int j = 0; j < 4; ++j) {
        const T* row = img.row(std::clamp(iy - 1 + j, 0, img.height - 1));
        float racc[3] = {0, 0, 0};
        for (int i = 0; i < 4; ++i) {
            const int xi = std::clamp(ix - 1 + i, 0, img.width - 1);
            for (int c = 0; c < 3; ++c) racc[c] += wx[i] * to_float(row[xi * 3 + c]);
        }
        for (int c = 0; c < 3; ++c) acc[c] += wy[j] * racc[c];
    }
    // オーバーシュートで負にならないようにする（リニア値なので 0 未満は無意味）
    for (int c = 0; c < 3; ++c) out[c] = std::max(acc[c], 0.0f);
}

// 画像の外（傾き補正でできる余白）。端の画素を引き伸ばさず黒にする。
// 通常は自動クロップで余白を描かないが、クロップモードでは全体を描くので見える（5.6 章）
template <class T>
inline bool outside(const Image<T>& img, double x, double y) {
    constexpr double kMargin = 0.5;  // 端の画素の外側半分までは画像の内とみなす
    return x < -kMargin || y < -kMargin || x > img.width + kMargin || y > img.height + kMargin;
}

// 1 行分をサンプリング（5.4 (1)）
template <class T>
void sample_row(const Image<T>& img, const Affine& out_to_src, int y, int w, Interpolation interp, float* dst) {
    PointD p = out_to_src.apply({0.5, y + 0.5});
    const double dx = out_to_src.a, dy = out_to_src.d;
    for (int x = 0; x < w; ++x, p.x += dx, p.y += dy) {
        float* o = dst + x * 3;
        if (outside(img, p.x, p.y)) {
            o[0] = o[1] = o[2] = 0.0f;
        } else if (interp == Interpolation::Bilinear) {
            sample_bilinear(img, p.x, p.y, o);
        } else {
            sample_bicubic(img, p.x, p.y, o);
        }
    }
}


template <class RowFn>
bool for_rows(int h, const CancelToken& cancel, RowFn fn) {
    ThreadPool::shared().parallel_for(h, kRowBlock, [&](int y0, int y1) {
        if (cancel.cancelled()) return;  // 行ブロックの切れ目で打ち切る
        for (int y = y0; y < y1; ++y) fn(y);
    });
    return !cancel.cancelled();
}

// 作業用バッファの使い回し。大きな確保は毎回 OS に返されてページフォールトが重いので、スレッドごとに持ち続ける。
// ただし書き出し（原寸）のような大きなものは、使い終わったら解放する
constexpr size_t kMaxRetainedFloats = 16u << 20;  // 64MB

struct Scratch {
    std::vector<float>& v;
    explicit Scratch(std::vector<float>& tl, size_t n) : v(tl) {
        if (v.size() < n) v.resize(n);
    }
    ~Scratch() {
        if (v.size() > kMaxRetainedFloats) v = {};
    }
};

// 周辺画素を使う処理（5.4 章 (5a): ノイズ低減 → 明瞭度 → シャープネス）ありの (1)〜(5b)。
// (origin, w, h) の外側に余白を足して (5) まで描き、(5a) をかけてから切り出して (5b) を行い、
// 行ごとに sink(y, row) に渡す（row は書き換えてよい）。余白は出力画像の範囲内だけ（外側は端が続くとみなす）
template <class Sink>
bool render_neighborhood(const SourceView& src, const GeometryPlan& plan, double scale, PointD origin, int w, int h,
                    const ColorPipeline& pipeline, Interpolation interp, const CancelToken& cancel, Sink sink) {
    const NeighborhoodRegion nr = plan_neighborhood(src, plan, scale, origin, w, h, pipeline);
    const double sigma = nr.sigma;
    const int ox = nr.ox, oy = nr.oy, left = nr.left, top = nr.top, bw = nr.bw, bh = nr.bh;

    // ラムダはワーカースレッドで動くので、thread_local は呼び出し元のインスタンスへの参照を取ってから使う
    thread_local std::vector<float> tl_buf;
    const Scratch scratch(tl_buf, static_cast<size_t>(bw) * bh * 3);
    float* buf = scratch.v.data();
    const Affine m = output_to_source(src, plan, scale, {origin.x - left, origin.y - top});
    if (!for_rows(bh, cancel, [&](int y) {
            float* row = buf + static_cast<size_t>(y) * bw * 3;
            if (src.full)
                sample_row(*src.full, m, y, bw, interp, row);
            else
                sample_row(*src.proxy, m, y, bw, interp, row);
            pipeline.process_tone(std::span<float>(row, static_cast<size_t>(bw) * 3));
        }))
        return false;
    if (!apply_noise_reduction(buf, bw, bh, pipeline.detail(), scale, ox - left, oy - top, cancel)) return false;
    if (pipeline.clarity() != 0.0f &&
        !apply_local_contrast(buf, bw, bh, pipeline.clarity(), sigma, ox - left, oy - top, cancel))
        return false;
    if (!apply_sharpen(buf, bw, bh, pipeline.detail(), scale, cancel)) return false;
    return for_rows(h, cancel, [&](int y) {
        float* row = buf + (static_cast<size_t>(y + top) * bw + left) * 3;
        pipeline.process_color(std::span<float>(row, static_cast<size_t>(w) * 3));
        sink(y, row);
    });
}

} // namespace

Affine output_to_source(const SourceView& src, const GeometryPlan& plan, double scale, PointD origin) {
    const Affine to_sensor = plan.output_to_sensor(scale, origin);
    const Affine sensor_to_src = Affine::scale(static_cast<double>(src.width()) / src.sensor_w,
                                               static_cast<double>(src.height()) / src.sensor_h);
    return sensor_to_src.after(to_sensor);
}

NeighborhoodRegion plan_neighborhood(const SourceView& src, const GeometryPlan& plan, double scale, PointD origin,
                                     int w, int h, const ColorPipeline& pipeline) {
    NeighborhoodRegion r;
    r.sigma = kClaritySigmaFraction * std::max(src.sensor_w, src.sensor_h) * scale;
    // 縮尺はフル解像度に対するもの（ノイズ低減・シャープネスの半径はフル解像度の画素で決める）。
    // プロキシから描くときもセンサー座標に対する縮尺なのでそのまま使える
    const int margin = std::max(pipeline.clarity() != 0.0f ? local_contrast_margin(r.sigma) : 0,
                                detail_margin(pipeline.detail(), scale));
    const int bound_w = static_cast<int>(std::lround(plan.output_width() * scale));
    const int bound_h = static_cast<int>(std::lround(plan.output_height() * scale));
    r.ox = static_cast<int>(std::floor(origin.x));
    r.oy = static_cast<int>(std::floor(origin.y));
    r.left = std::clamp(r.ox, 0, margin);
    r.top = std::clamp(r.oy, 0, margin);
    r.right = std::clamp(bound_w - r.ox - w, 0, margin);
    r.bottom = std::clamp(bound_h - r.oy - h, 0, margin);
    r.bw = w + r.left + r.right;
    r.bh = h + r.top + r.bottom;
    return r;
}

bool render_linear(const SourceView& src, const GeometryPlan& plan, double scale, PointD origin, int w, int h,
                   const ColorPipeline& pipeline, Interpolation interp, float* out, const CancelToken& cancel) {
    if (pipeline.needs_neighborhood())
        return render_neighborhood(src, plan, scale, origin, w, h, pipeline, interp, cancel, [&](int y, const float* row) {
            std::copy_n(row, static_cast<size_t>(w) * 3, out + static_cast<size_t>(y) * w * 3);
        });
    const Affine m = output_to_source(src, plan, scale, origin);
    return for_rows(h, cancel, [&](int y) {
        float* row = out + static_cast<size_t>(y) * w * 3;
        if (src.full)
            sample_row(*src.full, m, y, w, interp, row);
        else
            sample_row(*src.proxy, m, y, w, interp, row);
        pipeline.process(std::span<float>(row, static_cast<size_t>(w) * 3));
    });
}

bool render_display(const SourceView& src, const GeometryPlan& plan, double scale, PointD origin, int w, int h,
                    const ColorPipeline& pipeline, const OutputTransform& transform, Interpolation interp,
                    uint8_t* out, size_t stride, const CancelToken& cancel, PixelLayout layout) {
    if (transform.depth() != OutputDepth::U8) throw Error(Error::Code::InvalidArgument, "display needs U8");
    if (pipeline.needs_neighborhood())
        return render_neighborhood(src, plan, scale, origin, w, h, pipeline, interp, cancel, [&](int y, const float* row) {
            uint8_t* dst = out + static_cast<size_t>(y) * stride;
            if (layout == PixelLayout::Bgrx8)
                transform.apply_bgrx(row, dst, static_cast<size_t>(w));
            else
                transform.apply(row, dst, static_cast<size_t>(w));
        });
    const Affine m = output_to_source(src, plan, scale, origin);
    ThreadPool::shared().parallel_for(h, kRowBlock, [&](int y0, int y1) {
        if (cancel.cancelled()) return;
        std::vector<float> row(static_cast<size_t>(w) * 3);
        for (int y = y0; y < y1; ++y) {
            if (src.full)
                sample_row(*src.full, m, y, w, interp, row.data());
            else
                sample_row(*src.proxy, m, y, w, interp, row.data());
            pipeline.process(row);
            uint8_t* dst = out + static_cast<size_t>(y) * stride;
            if (layout == PixelLayout::Bgrx8)
                transform.apply_bgrx(row.data(), dst, static_cast<size_t>(w));
            else
                transform.apply(row.data(), dst, static_cast<size_t>(w));
        }
    });
    return !cancel.cancelled();
}

ImageF render_for_export(const DecodedRaw& raw, const Settings& settings, int long_edge) {
    const ColorPipeline pipeline(settings, raw.color);
    const GeometryPlan plan(raw.image.width, raw.image.height, raw.flip, settings.geometry);
    const int w = std::max(1, static_cast<int>(std::lround(plan.output_width())));
    const int h = std::max(1, static_cast<int>(std::lround(plan.output_height())));

    ImageF full(w, h);
    const SourceView src{&raw.image, nullptr, raw.image.width, raw.image.height};
    render_linear(src, plan, 1.0, {}, w, h, pipeline, Interpolation::Bicubic, full.data.data());

    int ow, oh;
    fit_long_edge(w, h, long_edge, ow, oh);
    if (ow == w && oh == h) return full;
    return resize_lanczos3(full, ow, oh);
}

ImageU8 render_preview(const DecodedRaw& raw, const ImageF& proxy, const Settings& settings,
                       const OutputTransform& transform) {
    const ColorPipeline pipeline(settings, raw.color);
    const GeometryPlan plan(raw.image.width, raw.image.height, raw.flip, settings.geometry);
    const double scale = static_cast<double>(proxy.width) / raw.image.width;
    const int w = std::max(1, static_cast<int>(std::lround(plan.output_width() * scale)));
    const int h = std::max(1, static_cast<int>(std::lround(plan.output_height() * scale)));
    ImageU8 out(w, h);
    const SourceView src{nullptr, &proxy, raw.image.width, raw.image.height};
    render_display(src, plan, scale, {}, w, h, pipeline, transform, Interpolation::Bilinear, out.data.data(),
                   static_cast<size_t>(w) * 3);
    return out;
}

namespace {

template <class T>
Image<T> encode(const ImageF& linear, const OutputTransform& transform) {
    Image<T> out(linear.width, linear.height);
    ThreadPool::shared().parallel_for(linear.height, kRowBlock, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) transform.apply(linear.row(y), out.row(y), static_cast<size_t>(linear.width));
    });
    return out;
}

} // namespace

ImageU8 encode_u8(const ImageF& linear, const OutputTransform& transform) {
    if (transform.depth() != OutputDepth::U8) throw Error(Error::Code::InvalidArgument, "transform is not U8");
    return encode<uint8_t>(linear, transform);
}

ImageU16 encode_u16(const ImageF& linear, const OutputTransform& transform) {
    if (transform.depth() != OutputDepth::U16) throw Error(Error::Code::InvalidArgument, "transform is not U16");
    return encode<uint16_t>(linear, transform);
}

} // namespace focal
