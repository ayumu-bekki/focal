#include "imaging/local_contrast.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cmath>
#include <vector>

namespace focal {

namespace {

// Rec.2020 の輝度の係数
constexpr float kLr = 0.2627f, kLg = 0.6780f, kLb = 0.0593f;
using namespace local_contrast;

constexpr int kRowBlock = 16;

// 2^x の近似（小数部を多項式、整数部を指数部に入れる。相対誤差 1e-4 以下）
inline float fast_exp2(float x) {
    const float xi = std::floor(x);
    const float f = x - xi;
    // 2^f（0 ≤ f < 1）の 4 次多項式
    const float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * 0.0096181f)));
    // 2^xi は指数部に直接入れる（ldexp は遅い）
    const int e = std::clamp(static_cast<int>(xi), -126, 127) + 127;
    return p * std::bit_cast<float>(static_cast<uint32_t>(e) << 23);
}
constexpr int kColBlock = 64;

// 1 行の箱型ぼかし（半径 r、端は端の値が続くとみなす）。src と dst は別の領域
void box_row(const float* src, float* dst, int n, int r, size_t step) {
    const float inv = 1.0f / (2 * r + 1);
    auto at = [&](int i) { return src[static_cast<size_t>(std::clamp(i, 0, n - 1)) * step]; };
    float sum = 0;
    for (int i = -r; i <= r; ++i) sum += at(i);
    for (int i = 0; i < n; ++i) {
        dst[static_cast<size_t>(i) * step] = sum * inv;
        sum += at(i + r + 1) - at(i - r);
    }
}

// 箱型ぼかしを 3 回（ガウスぼかしの近似、半径に依らない速さ）
void gaussian_approx(std::vector<float>& img, int w, int h, int r, const CancelToken& cancel) {
    std::vector<float> tmp(img.size());
    auto& pool = ThreadPool::shared();
    for (int pass = 0; pass < 3; ++pass) {
        pool.parallel_for(h, kRowBlock, [&](int y0, int y1) {
            if (cancel.cancelled()) return;
            for (int y = y0; y < y1; ++y) {
                const size_t o = static_cast<size_t>(y) * w;
                box_row(img.data() + o, tmp.data() + o, w, r, 1);
            }
        });
        pool.parallel_for((w + kColBlock - 1) / kColBlock, 1, [&](int b0, int b1) {
            if (cancel.cancelled()) return;
            for (int x = b0 * kColBlock; x < std::min(w, b1 * kColBlock); ++x)
                box_row(tmp.data() + x, img.data() + x, h, r, static_cast<size_t>(w));
        });
    }
}

} // namespace

int local_contrast_margin(double sigma) { return static_cast<int>(std::ceil(3.0 * sigma)) + 1; }

LocalContrastPlan plan_local_contrast(double sigma, int origin_x, int origin_y, int w, int h) {
    // σ が大きいので縮小（面積平均）してから計算する。縮小率は σ の 1/4 以下（最大 1/8）。
    // 縮小の区切りは出力画像の座標で f の倍数に合わせる（描く範囲に依らず同じ結果にする）
    LocalContrastPlan p;
    p.f = std::clamp(static_cast<int>(sigma / 4.0), 1, 8);
    auto floor_div = [](int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
    p.gx0 = floor_div(origin_x, p.f);
    p.gy0 = floor_div(origin_y, p.f);
    p.sw = floor_div(origin_x + w - 1, p.f) - p.gx0 + 1;
    p.sh = floor_div(origin_y + h - 1, p.f) - p.gy0 + 1;
    // 箱型 3 回で σ / f になる半径
    const double ss = sigma / p.f;
    p.r = static_cast<int>(std::lround((std::sqrt(4.0 * ss * ss + 1.0) - 1.0) / 2.0));
    return p;
}

bool apply_local_contrast(float* rgb, int w, int h, float amount, double sigma, int origin_x, int origin_y,
                          const CancelToken& cancel) {
    if (amount == 0.0f || w <= 0 || h <= 0 || sigma < 0.5) return !cancel.cancelled();
    auto& pool = ThreadPool::shared();

    // 輝度の対数（EV）
    const size_t n = static_cast<size_t>(w) * h;
    // 作業用バッファはスレッドごとに使い回す（大きな確保は毎回 OS に返され、ページフォールトが重い）。
    // ラムダはワーカースレッドで動くので、呼び出し元のインスタンスへの参照を取ってから使う
    thread_local std::vector<float> tl_lum;
    std::vector<float>& lum = tl_lum;
    if (lum.size() < n) lum.resize(n);
    struct Trim {  // 書き出し（原寸）のような大きなものは、使い終わったら解放する
        std::vector<float>& v;
        ~Trim() {
            if (v.size() > (16u << 20)) v = {};
        }
    } trim{lum};
    pool.parallel_for(h, kRowBlock, [&](int y0, int y1) {
        for (size_t i = static_cast<size_t>(y0) * w; i < static_cast<size_t>(y1) * w; ++i) {
            const float* p = rgb + i * 3;
            lum[i] = std::log2(std::max(kLr * p[0] + kLg * p[1] + kLb * p[2], kMinLum));
        }
    });

    // 縮小して計算し、双線形で戻す（区切りは plan_local_contrast）
    const LocalContrastPlan plan = plan_local_contrast(sigma, origin_x, origin_y, w, h);
    const int f = plan.f, gx0 = plan.gx0, gy0 = plan.gy0, sw = plan.sw, sh = plan.sh;
    thread_local std::vector<float> tl_small;
    std::vector<float>& small = tl_small;
    small.resize(static_cast<size_t>(sw) * sh);
    pool.parallel_for(sh, kRowBlock, [&](int y0, int y1) {
        for (int sy = y0; sy < y1; ++sy) {
            const int ya = std::max(0, (gy0 + sy) * f - origin_y), yb = std::min(h, (gy0 + sy + 1) * f - origin_y);
            for (int sx = 0; sx < sw; ++sx) {
                const int xa = std::max(0, (gx0 + sx) * f - origin_x), xb = std::min(w, (gx0 + sx + 1) * f - origin_x);
                float sum = 0;
                for (int y = ya; y < yb; ++y)
                    for (int x = xa; x < xb; ++x) sum += lum[static_cast<size_t>(y) * w + x];
                small[static_cast<size_t>(sy) * sw + sx] = sum / static_cast<float>((yb - ya) * (xb - xa));
            }
        }
    });
    // エッジを保つぼかし（ガイデッドフィルタ、輝度自身をガイドにする）。大きな明暗の境界（分散が kEdgeEps より大きい）は
    // ぼかさずに残し、それより細かい明暗だけを「局所的な明暗」として取り出す。普通のぼかしだと境界の両側にハローが出る
    const int r = plan.r;
    const size_t sn = small.size();
    thread_local std::vector<float> tl_a, tl_b, tl_ii;
    std::vector<float>&a = tl_a, &b = tl_b, &ii = tl_ii;
    a = small;  // 平均
    ii.resize(sn);
    for (size_t i = 0; i < sn; ++i) ii[i] = small[i] * small[i];
    if (r >= 1) {
        gaussian_approx(a, sw, sh, r, cancel);
        gaussian_approx(ii, sw, sh, r, cancel);
    }
    b.resize(sn);
    for (size_t i = 0; i < sn; ++i) {
        const float var = std::max(0.0f, ii[i] - a[i] * a[i]);
        const float k = var / (var + kEdgeEps);
        b[i] = a[i] * (1.0f - k);  // b = mean - k × mean
        a[i] = k;
    }
    if (r >= 1) {
        gaussian_approx(a, sw, sh, r, cancel);
        gaussian_approx(b, sw, sh, r, cancel);
    }
    if (cancel.cancelled()) return false;

    // 列ごとの補間位置（縮小画像上の座標。画素中心を合わせる）
    std::vector<int> cx0(w), cx1(w);
    std::vector<float> ctx(w);
    const float inv_f = 1.0f / f;
    for (int x = 0; x < w; ++x) {
        const float fx = std::clamp((origin_x + x + 0.5f) * inv_f - 0.5f - gx0, 0.0f, static_cast<float>(sw - 1));
        cx0[x] = static_cast<int>(fx);
        cx1[x] = std::min(cx0[x] + 1, sw - 1);
        ctx[x] = fx - cx0[x];
    }

    pool.parallel_for(h, kRowBlock, [&](int y0, int y1) {
        if (cancel.cancelled()) return;
        std::vector<float> ratio(w);
        for (int y = y0; y < y1; ++y) {
            const float fy = std::clamp((origin_y + y + 0.5f) * inv_f - 0.5f - gy0, 0.0f, static_cast<float>(sh - 1));
            const int iy0 = static_cast<int>(fy), iy1 = std::min(iy0 + 1, sh - 1);
            const float ty = fy - iy0;
            const float* a0 = a.data() + static_cast<size_t>(iy0) * sw;
            const float* a1 = a.data() + static_cast<size_t>(iy1) * sw;
            const float* b0 = b.data() + static_cast<size_t>(iy0) * sw;
            const float* b1 = b.data() + static_cast<size_t>(iy1) * sw;
            const float* lrow = lum.data() + static_cast<size_t>(y) * w;
            for (int x = 0; x < w; ++x) {
                // 係数 a, b を双線形で戻し、ぼかした値 = a × l + b
                const float at = a0[cx0[x]] + (a0[cx1[x]] - a0[cx0[x]]) * ctx[x];
                const float ab = a1[cx0[x]] + (a1[cx1[x]] - a1[cx0[x]]) * ctx[x];
                const float bt = b0[cx0[x]] + (b0[cx1[x]] - b0[cx0[x]]) * ctx[x];
                const float bb = b1[cx0[x]] + (b1[cx1[x]] - b1[cx0[x]]) * ctx[x];
                const float l = lrow[x];
                const float blur = (at + (ab - at) * ty) * l + (bt + (bb - bt) * ty);
                // 局所的な明暗（EV）。大きな段差でハローが出ないよう、1 EV を超える分は弱める
                const float d = l - blur;
                // 中間調ほど強く（黒つぶれ・白飛びの近くは弱く）
                const float t = (l - kMidLog) * (1.0f / kMidWidth);
                const float t2 = t * t;
                const float wmid = 1.0f / (1.0f + t2 * t2);
                // 正: 局所的な明暗を強める（大きな差は d / (1 + |d|) で抑える）。
                // 負: 局所的な明暗を 0 に近づける（-100 で半分、-200 で中間調はほぼ平らに）。反転はさせない
                ratio[x] = amount > 0.0f ? kStrength * amount * wmid * d / (1.0f + std::abs(d))
                                         : 0.5f * amount * wmid * d;
            }
            float* p = rgb + static_cast<size_t>(y) * w * 3;
            for (int x = 0; x < w; ++x, p += 3) {
                float k = fast_exp2(ratio[x]);
                if (k > 1.0f) {
                    // 新たに白飛びさせない
                    const float mx = std::max(p[0], std::max(p[1], p[2]));
                    if (mx * k > 1.0f) k = std::max(1.0f, 1.0f / mx);
                }
                p[0] *= k;
                p[1] *= k;
                p[2] *= k;
            }
        }
    });
    return !cancel.cancelled();
}

} // namespace focal
