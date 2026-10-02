#include "imaging/resample.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include "util/thread_pool.h"

namespace focal {

namespace {

// 1 次元の重み表。出力 i は src[first[i] .. first[i]+count[i]) を weights で合成する。
struct Taps {
    std::vector<int> first;
    std::vector<int> count;
    std::vector<float> weights;  // 出力ごとに max_taps 個
    int max_taps = 0;
};

// 面積平均（ボックスフィルタの正確な被覆率）
Taps area_taps(int src_n, int dst_n) {
    Taps t;
    const double scale = static_cast<double>(src_n) / dst_n;
    t.max_taps = static_cast<int>(std::ceil(scale)) + 2;
    t.first.resize(dst_n);
    t.count.resize(dst_n);
    t.weights.assign(static_cast<size_t>(dst_n) * t.max_taps, 0.0f);
    for (int i = 0; i < dst_n; ++i) {
        const double a = i * scale, b = (i + 1) * scale;
        const int s0 = static_cast<int>(std::floor(a));
        const int s1 = std::min(src_n, static_cast<int>(std::ceil(b)));
        t.first[i] = s0;
        t.count[i] = s1 - s0;
        for (int s = s0; s < s1; ++s) {
            const double cover = std::min<double>(b, s + 1) - std::max<double>(a, s);
            t.weights[static_cast<size_t>(i) * t.max_taps + (s - s0)] = static_cast<float>(cover / scale);
        }
    }
    return t;
}

double lanczos3(double x) {
    x = std::abs(x);
    if (x < 1e-8) return 1.0;
    if (x >= 3.0) return 0.0;
    const double px = std::numbers::pi * x;
    return 3.0 * std::sin(px) * std::sin(px / 3.0) / (px * px);
}

Taps lanczos_taps(int src_n, int dst_n) {
    Taps t;
    const double scale = static_cast<double>(src_n) / dst_n;
    const double support = 3.0 * std::max(1.0, scale);
    const double fscale = std::max(1.0, scale);
    t.max_taps = static_cast<int>(std::ceil(support * 2)) + 2;
    t.first.resize(dst_n);
    t.count.resize(dst_n);
    t.weights.assign(static_cast<size_t>(dst_n) * t.max_taps, 0.0f);
    for (int i = 0; i < dst_n; ++i) {
        const double center = (i + 0.5) * scale;
        int s0 = static_cast<int>(std::floor(center - support));
        int s1 = static_cast<int>(std::ceil(center + support));
        s0 = std::max(0, s0);
        s1 = std::min(src_n, s1);
        s1 = std::min(s1, s0 + t.max_taps);
        double sum = 0;
        float* w = &t.weights[static_cast<size_t>(i) * t.max_taps];
        for (int s = s0; s < s1; ++s) {
            const double v = lanczos3((s + 0.5 - center) / fscale);
            w[s - s0] = static_cast<float>(v);
            sum += v;
        }
        for (int s = s0; s < s1; ++s) w[s - s0] = static_cast<float>(w[s - s0] / sum);
        t.first[i] = s0;
        t.count[i] = s1 - s0;
    }
    return t;
}

// 縦方向に合成してから横方向に合成する。出力行ごとに並列化。
template <class Src, class Convert>
ImageF separable_resample(const Image<Src>& src, int ow, int oh, const Taps& tx, const Taps& ty,
                          Convert convert) {
    ImageF out(ow, oh);
    ThreadPool::shared().parallel_for(oh, 8, [&](int y0, int y1) {
        std::vector<float> line(static_cast<size_t>(src.width) * 3);
        for (int oy = y0; oy < y1; ++oy) {
            std::fill(line.begin(), line.end(), 0.0f);
            const float* wy = &ty.weights[static_cast<size_t>(oy) * ty.max_taps];
            for (int k = 0; k < ty.count[oy]; ++k) {
                const Src* srow = src.row(ty.first[oy] + k);
                const float wk = wy[k];
                for (size_t i = 0; i < line.size(); ++i) line[i] += wk * convert(srow[i]);
            }
            float* orow = out.row(oy);
            for (int ox = 0; ox < ow; ++ox) {
                const float* wx = &tx.weights[static_cast<size_t>(ox) * tx.max_taps];
                const float* l = &line[static_cast<size_t>(tx.first[ox]) * 3];
                float r = 0, g = 0, b = 0;
                for (int k = 0; k < tx.count[ox]; ++k) {
                    r += wx[k] * l[k * 3 + 0];
                    g += wx[k] * l[k * 3 + 1];
                    b += wx[k] * l[k * 3 + 2];
                }
                orow[ox * 3 + 0] = r;
                orow[ox * 3 + 1] = g;
                orow[ox * 3 + 2] = b;
            }
        }
    });
    return out;
}

} // namespace

void fit_long_edge(int w, int h, int max_long_edge, int& out_w, int& out_h) {
    const int long_edge = std::max(w, h);
    if (max_long_edge <= 0 || long_edge <= max_long_edge) {
        out_w = w;
        out_h = h;
        return;
    }
    const double s = static_cast<double>(max_long_edge) / long_edge;
    out_w = std::max(1, static_cast<int>(std::lround(w * s)));
    out_h = std::max(1, static_cast<int>(std::lround(h * s)));
}

ImageF make_proxy(const ImageU16& full, int max_long_edge) {
    int ow, oh;
    fit_long_edge(full.width, full.height, max_long_edge, ow, oh);
    constexpr float kInv = 1.0f / 65535.0f;
    return separable_resample(full, ow, oh, area_taps(full.width, ow), area_taps(full.height, oh),
                              [](uint16_t v) { return v * kInv; });
}

ImageF resize_lanczos3(const ImageF& src, int out_w, int out_h) {
    return separable_resample(src, out_w, out_h, lanczos_taps(src.width, out_w), lanczos_taps(src.height, out_h),
                              [](float v) { return v; });
}

} // namespace focal
