#include "imaging/detail.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace focal {

namespace {

using namespace detail;

// フル解像度の画素での半径（v3.13 で決めた値）
constexpr double kSharpenSigma = 1.0;      // シャープネスのぼかしの σ
constexpr double kLumaRadius = 3.0;        // 輝度のノイズ低減の窓の半径（高感度の粒は 2〜3 画素にまたがる）
constexpr double kChromaRadiusMax = 32.0;  // カラーのノイズ低減の窓の半径（量 1 のとき。高感度の色むらは 5〜15 画素と大きい）
constexpr double kMinSigma = 0.5;          // これより小さい（縮小表示）ときは行わない（見た目にほぼ出ない）
constexpr double kMinChromaRadius = 2.0;   // カラーのノイズ低減は、縮小で色のまだらがほぼ平均されるので窓の半径 2 画素未満では行わない

constexpr int kRowBlock = 16;
constexpr int kPlanes = 9;

// 作業用の平面。スレッドごとに使い回す（大きな確保は毎回 OS に返されてページフォールトが重い）。
// ラムダはワーカースレッドで動くので、返した参照を取ってから使うこと
std::vector<float>& plane(int slot, size_t n) {
    thread_local std::array<std::vector<float>, kPlanes> planes;
    auto& v = planes[static_cast<size_t>(slot)];
    if (v.size() < n) v.resize(n);
    return v;
}

void trim_planes() {
    for (int i = 0; i < kPlanes; ++i) {
        auto& v = plane(i, 0);
        if (v.size() > (16u << 20)) v = {};  // 書き出し（原寸）のような大きなものは解放する
    }
}

struct TrimGuard {
    ~TrimGuard() { trim_planes(); }
};

// 分離可能な畳み込み（カーネルは対称、長さ 2r+1）。src → dst、tmp は作業用。端は端の値が続くとみなす
void convolve(const float* src, float* dst, float* tmp, int w, int h, const std::vector<float>& k,
              const CancelToken& cancel) {
    const int r = static_cast<int>(k.size() / 2);
    auto& pool = ThreadPool::shared();
    pool.parallel_for(h, kRowBlock, [&](int y0, int y1) {
        if (cancel.cancelled()) return;
        for (int y = y0; y < y1; ++y) {
            const float* s = src + static_cast<size_t>(y) * w;
            float* t = tmp + static_cast<size_t>(y) * w;
            // 端だけ範囲を確かめ、内側はそのまま読む
            const int x_in0 = std::min(r, w), x_in1 = std::max(x_in0, w - r);
            for (int x = 0; x < x_in0; ++x) {
                float acc = 0;
                for (int j = -r; j <= r; ++j) acc += k[static_cast<size_t>(j + r)] * s[std::clamp(x + j, 0, w - 1)];
                t[x] = acc;
            }
            // 内側は x 方向に連続（ベクトル化しやすい並び）
            std::fill(t + x_in0, t + x_in1, 0.0f);
            for (int j = 0; j <= 2 * r; ++j) {
                const float kj = k[static_cast<size_t>(j)];
                const float* q = s + j - r;
                for (int x = x_in0; x < x_in1; ++x) t[x] += kj * q[x];
            }
            for (int x = x_in1; x < w; ++x) {
                float acc = 0;
                for (int j = -r; j <= r; ++j) acc += k[static_cast<size_t>(j + r)] * s[std::clamp(x + j, 0, w - 1)];
                t[x] = acc;
            }
        }
    });
    // 縦方向は行ベクトルの和で（連続アクセスになる）
    pool.parallel_for(h, kRowBlock, [&](int y0, int y1) {
        if (cancel.cancelled()) return;
        for (int y = y0; y < y1; ++y) {
            float* d = dst + static_cast<size_t>(y) * w;
            std::fill(d, d + w, 0.0f);
            for (int j = -r; j <= r; ++j) {
                const float kj = k[static_cast<size_t>(j + r)];
                const float* t = tmp + static_cast<size_t>(std::clamp(y + j, 0, h - 1)) * w;
                for (int x = 0; x < w; ++x) d[x] += kj * t[x];
            }
        }
    });
}

std::vector<float> gaussian_kernel(double sigma) {
    const int r = std::max(1, static_cast<int>(std::ceil(3.0 * sigma)));
    std::vector<float> k(static_cast<size_t>(2 * r + 1));
    double sum = 0;
    for (int i = -r; i <= r; ++i) sum += k[static_cast<size_t>(i + r)] = static_cast<float>(std::exp(-0.5 * i * i / (sigma * sigma)));
    for (auto& v : k) v = static_cast<float>(v / sum);
    return k;
}

// 箱型の平均（半径 r、累積和で半径に依らない速さ）。src → dst、tmp は作業用。端は端の値が続くとみなす
void box_mean(const float* src, float* dst, float* tmp, int w, int h, int r, const CancelToken& cancel) {
    const float inv = 1.0f / (2 * r + 1);
    auto& pool = ThreadPool::shared();
    pool.parallel_for(h, kRowBlock, [&](int y0, int y1) {
        if (cancel.cancelled()) return;
        for (int y = y0; y < y1; ++y) {
            const float* s = src + static_cast<size_t>(y) * w;
            float* t = tmp + static_cast<size_t>(y) * w;
            float sum = 0;
            for (int j = -r; j <= r; ++j) sum += s[std::clamp(j, 0, w - 1)];
            for (int x = 0; x < w; ++x) {
                t[x] = sum * inv;
                sum += s[std::min(x + r + 1, w - 1)] - s[std::max(x - r, 0)];
            }
        }
    });
    // 縦方向: 行ブロックごとに、最初の行の和を作ってから行ベクトルを足し引きする
    pool.parallel_for(h, kRowBlock, [&](int y0, int y1) {
        if (cancel.cancelled()) return;
        std::vector<float> sum(static_cast<size_t>(w), 0.0f);
        auto row = [&](int y) { return tmp + static_cast<size_t>(std::clamp(y, 0, h - 1)) * w; };
        for (int j = y0 - r; j <= y0 + r; ++j) {
            const float* t = row(j);
            for (int x = 0; x < w; ++x) sum[x] += t[x];
        }
        for (int y = y0; y < y1; ++y) {
            float* d = dst + static_cast<size_t>(y) * w;
            const float* add = row(y + r + 1);
            const float* sub = row(y - r);
            for (int x = 0; x < w; ++x) {
                d[x] = sum[x] * inv;
                sum[x] += add[x] - sub[x];
            }
        }
    });
}

// 輝度の平方根（暗部のノイズを明部と同じくらいの大きさで扱える、知覚に近い尺度）
inline float lum_of(const float* p) { return std::max(0.0f, kLr * p[0] + kLg * p[1] + kLb * p[2]); }

// 輝度を new_sqrt^2 にするよう RGB に同じ比率を掛ける。brighten_cap なら新たに白飛びさせない。
// 分岐を使わない（ループがベクトル化されるように）
inline void set_lum(float* p, float old_sqrt, float new_sqrt, bool brighten_cap) {
    const float old_l = old_sqrt * old_sqrt;
    const float ns = std::max(0.0f, new_sqrt);
    float ratio = old_l > 1e-12f ? ns * ns / std::max(old_l, 1e-12f) : 1.0f;
    if (brighten_cap) {
        // 比率の上限は「最大チャンネルが 1 になるまで」（1 未満にはしない）
        const float mx = std::max(p[0], std::max(p[1], p[2]));
        ratio = std::min(ratio, std::max(1.0f, 1.0f / std::max(mx, 1e-6f)));
    }
    p[0] *= ratio;
    p[1] *= ratio;
    p[2] *= ratio;
}

} // namespace

namespace detail {

// 縮小表示では粒が平均されてほぼ見えないので、窓の半径が 1.5 画素未満なら行わない
int luma_radius(double scale) {
    const double r = kLumaRadius * scale;
    return r < 1.5 ? 0 : static_cast<int>(std::lround(r));
}

int chroma_radius(const DetailParams& p, double scale) {
    const double r = kChromaRadiusMax * p.color_noise_reduction * scale;
    return r < kMinChromaRadius ? 0 : std::max(1, static_cast<int>(std::lround(r)));
}

// カラーのガイデッドフィルタの係数を計算する解像度（1/f）。色の変化はなめらかなので縮小しても見た目はほぼ同じ。
// 縮小した画像の上での窓の半径が 2 前後になるようにする（最大 1/8）
int chroma_factor(int rc) { return rc >= 2 ? std::clamp((rc + 1) / 2, 2, 8) : 1; }

std::vector<float> sharpen_kernel(const DetailParams& p, double scale) {
    const double sigma = kSharpenSigma * scale;
    if (p.sharpness <= 0 || sigma < kMinSigma) return {};
    return gaussian_kernel(sigma);
}

} // namespace detail

int detail_margin(const DetailParams& p, double scale) {
    int m = 0;
    if (p.noise_reduction > 0) m = std::max(m, 2 * luma_radius(scale) + 1);
    if (p.color_noise_reduction > 0) {
        const int rc = chroma_radius(p, scale);
        m = std::max(m, 2 * rc + 2 * chroma_factor(rc) + 1);
    }
    if (p.sharpness > 0) m = std::max(m, static_cast<int>(std::ceil(3.0 * kSharpenSigma * scale)) + 1);
    return m;
}

bool apply_noise_reduction(float* rgb, int w, int h, const DetailParams& p, double scale, int origin_x, int origin_y,
                           const CancelToken& cancel) {
    const size_t n = static_cast<size_t>(w) * h;
    const TrimGuard trim;
    auto& pool = ThreadPool::shared();

    // 輝度: √輝度をガイデッドフィルタ（自身をガイド）で平らにする。差が ε より大きい輪郭は残る
    const int r = luma_radius(scale);
    if (p.noise_reduction > 0 && r >= 1) {
        std::vector<float>&s = plane(0, n), &ss = plane(1, n), &mean = plane(2, n), &tmp = plane(3, n),
                         &a = plane(4, n), &b = plane(5, n);
        pool.parallel_for(h, kRowBlock, [&](int y0, int y1) {
            for (size_t i = static_cast<size_t>(y0) * w; i < static_cast<size_t>(y1) * w; ++i) {
                s[i] = std::sqrt(lum_of(rgb + i * 3));
                ss[i] = s[i] * s[i];
            }
        });
        box_mean(s.data(), mean.data(), tmp.data(), w, h, r, cancel);
        box_mean(ss.data(), a.data(), tmp.data(), w, h, r, cancel);  // a ← 二乗の平均
        if (cancel.cancelled()) return false;
        const float eps = (kLumaEpsMax * p.noise_reduction) * (kLumaEpsMax * p.noise_reduction);
        pool.parallel_for(h, kRowBlock, [&](int y0, int y1) {
            for (size_t i = static_cast<size_t>(y0) * w; i < static_cast<size_t>(y1) * w; ++i) {
                const float var = std::max(0.0f, a[i] - mean[i] * mean[i]);
                const float ai = var / (var + eps);
                a[i] = ai;
                b[i] = mean[i] * (1.0f - ai);
            }
        });
        box_mean(a.data(), mean.data(), tmp.data(), w, h, r, cancel);  // mean ← a の平均
        box_mean(b.data(), ss.data(), tmp.data(), w, h, r, cancel);    // ss ← b の平均
        if (cancel.cancelled()) return false;
        pool.parallel_for(h, kRowBlock, [&](int y0, int y1) {
            for (size_t i = static_cast<size_t>(y0) * w; i < static_cast<size_t>(y1) * w; ++i)
                set_lum(rgb + i * 3, s[i], mean[i] * s[i] + ss[i], false);
        });
    }

    // カラー（輝度の後。ガイドの輝度のノイズが減っているほど、色のまだらと輝度の揺れを取り違えにくい）:
    // 輝度はそのままで、色の成分（R − Y、B − Y）を、√輝度をガイドにしたガイデッドフィルタで平らにする。
    // 窓の中で色を「輝度の一次式」で近似するので、色のまだら（輝度と無関係な色の揺れ）は消え、
    // 輝度に輪郭がある場所の色の境界（葉と背景など）は保たれる。
    // 係数（a, b）は 1/f に縮小して計算し、双線形で戻してから原寸の輝度に当てる（高速ガイデッドフィルタ）。
    // 縮小の区切りは出力画像の座標で f の倍数に合わせる
    const int rc = chroma_radius(p, scale);
    if (p.color_noise_reduction > 0 && rc >= 1) {
        const int f = chroma_factor(rc), rs = std::max(1, rc / f);
        auto floor_div = [](int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
        const int gx0 = floor_div(origin_x, f), gy0 = floor_div(origin_y, f);
        const int sw = floor_div(origin_x + w - 1, f) - gx0 + 1, sh = floor_div(origin_y + h - 1, f) - gy0 + 1;
        const size_t sn = static_cast<size_t>(sw) * sh;
        // 縮小（面積平均）: ガイド、ガイドの二乗、2 つの色、色 × ガイド。原寸の値は作業用の平面に書き出さず、その場で計算する
        std::vector<float>&lg = plane(3, sn), &lgg = plane(4, sn), &lc0 = plane(5, sn), &lc1 = plane(6, sn),
                         &lg0 = plane(7, sn), &lg1 = plane(8, sn);
        pool.parallel_for(sh, kRowBlock, [&](int y0, int y1) {
            for (int sy = y0; sy < y1; ++sy) {
                const int ya = std::max(0, (gy0 + sy) * f - origin_y), yb = std::min(h, (gy0 + sy + 1) * f - origin_y);
                for (int sx = 0; sx < sw; ++sx) {
                    const int xa = std::max(0, (gx0 + sx) * f - origin_x), xb = std::min(w, (gx0 + sx + 1) * f - origin_x);
                    float s_g = 0, s_gg = 0, s_c0 = 0, s_c1 = 0, s_g0 = 0, s_g1 = 0;
                    for (int y = ya; y < yb; ++y)
                        for (int x = xa; x < xb; ++x) {
                            const float* q = rgb + (static_cast<size_t>(y) * w + x) * 3;
                            const float yl = kLr * q[0] + kLg * q[1] + kLb * q[2];
                            const float g = std::sqrt(std::max(0.0f, yl)), c0 = q[0] - yl, c1 = q[2] - yl;
                            s_g += g;
                            s_gg += g * g;
                            s_c0 += c0;
                            s_c1 += c1;
                            s_g0 += c0 * g;
                            s_g1 += c1 * g;
                        }
                    const float inv = 1.0f / static_cast<float>((yb - ya) * (xb - xa));
                    const size_t j = static_cast<size_t>(sy) * sw + sx;
                    lg[j] = s_g * inv, lgg[j] = s_gg * inv, lc0[j] = s_c0 * inv, lc1[j] = s_c1 * inv;
                    lg0[j] = s_g0 * inv, lg1[j] = s_g1 * inv;
                }
            }
        });
        // 縮小画像の上で窓の平均を取り、係数を求める（作業用は小さいので普通に確保する）
        std::vector<float> tmp(sn), mg(sn), mgg(sn), m0(sn), m1(sn), mg0(sn), mg1(sn);
        box_mean(lg.data(), mg.data(), tmp.data(), sw, sh, rs, cancel);
        box_mean(lgg.data(), mgg.data(), tmp.data(), sw, sh, rs, cancel);
        box_mean(lc0.data(), m0.data(), tmp.data(), sw, sh, rs, cancel);
        box_mean(lc1.data(), m1.data(), tmp.data(), sw, sh, rs, cancel);
        box_mean(lg0.data(), mg0.data(), tmp.data(), sw, sh, rs, cancel);
        box_mean(lg1.data(), mg1.data(), tmp.data(), sw, sh, rs, cancel);
        if (cancel.cancelled()) return false;
        // lc0, lc1 ← a（R − Y、B − Y）、lg0, lg1 ← b
        for (size_t j = 0; j < sn; ++j) {
            const float var = std::max(0.0f, mgg[j] - mg[j] * mg[j]) + kChromaEps;
            const float a0 = (mg0[j] - mg[j] * m0[j]) / var, a1 = (mg1[j] - mg[j] * m1[j]) / var;
            lc0[j] = a0, lc1[j] = a1;
            lg0[j] = m0[j] - a0 * mg[j], lg1[j] = m1[j] - a1 * mg[j];
        }
        box_mean(lc0.data(), m0.data(), tmp.data(), sw, sh, rs, cancel);
        box_mean(lc1.data(), m1.data(), tmp.data(), sw, sh, rs, cancel);
        box_mean(lg0.data(), mg0.data(), tmp.data(), sw, sh, rs, cancel);
        box_mean(lg1.data(), mg1.data(), tmp.data(), sw, sh, rs, cancel);
        if (cancel.cancelled()) return false;
        // 係数を双線形で原寸に戻し、色 = a × ガイド + b。
        // 先に縮小画像の各行を横方向だけ原寸の幅に広げておき（4 つの係数）、原寸の各行では上下 2 行を補間する（連続アクセス）
        const float inv_f = 1.0f / f;
        std::vector<int> cx0(static_cast<size_t>(w)), cx1(static_cast<size_t>(w));
        std::vector<float> ctx(static_cast<size_t>(w));
        for (int x = 0; x < w; ++x) {
            const float fx = std::clamp((origin_x + x + 0.5f) * inv_f - 0.5f - gx0, 0.0f, static_cast<float>(sw - 1));
            cx0[x] = static_cast<int>(fx);
            cx1[x] = std::min(cx0[x] + 1, sw - 1);
            ctx[x] = fx - cx0[x];
        }
        std::vector<float>&u = plane(0, static_cast<size_t>(sh) * w * 4);
        const std::vector<float>* coef[4] = {&m0, &mg0, &m1, &mg1};
        pool.parallel_for(sh, kRowBlock, [&](int y0, int y1) {
            for (int sy = y0; sy < y1; ++sy)
                for (int k = 0; k < 4; ++k) {
                    const float* src = coef[k]->data() + static_cast<size_t>(sy) * sw;
                    float* dst = u.data() + (static_cast<size_t>(sy) * 4 + k) * w;
                    for (int x = 0; x < w; ++x) dst[x] = src[cx0[x]] + (src[cx1[x]] - src[cx0[x]]) * ctx[x];
                }
        });
        pool.parallel_for(h, kRowBlock, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const float fy = std::clamp((origin_y + y + 0.5f) * inv_f - 0.5f - gy0, 0.0f, static_cast<float>(sh - 1));
                const int iy0 = static_cast<int>(fy), iy1 = std::min(iy0 + 1, sh - 1);
                const float ty = fy - iy0;
                const float* t0 = u.data() + static_cast<size_t>(iy0) * 4 * w;
                const float* t1 = u.data() + static_cast<size_t>(iy1) * 4 * w;
                float* q = rgb + static_cast<size_t>(y) * w * 3;
                for (int x = 0; x < w; ++x, q += 3) {
                    const float a0 = t0[x] + (t1[x] - t0[x]) * ty;
                    const float b0 = t0[w + x] + (t1[w + x] - t0[w + x]) * ty;
                    const float a1 = t0[2 * w + x] + (t1[2 * w + x] - t0[2 * w + x]) * ty;
                    const float b1 = t0[3 * w + x] + (t1[3 * w + x] - t0[3 * w + x]) * ty;
                    const float yy = kLr * q[0] + kLg * q[1] + kLb * q[2];
                    const float g = std::sqrt(std::max(0.0f, yy));
                    const float r = std::max(0.0f, yy + a0 * g + b0), b = std::max(0.0f, yy + a1 * g + b1);
                    q[0] = r;
                    q[2] = b;
                    q[1] = std::max(0.0f, (yy - kLr * r - kLb * b) / kLg);
                }
            }
        });
    }
    return !cancel.cancelled();
}

bool apply_sharpen(float* rgb, int w, int h, const DetailParams& p, double scale, const CancelToken& cancel) {
    const std::vector<float> kernel = sharpen_kernel(p, scale);
    if (kernel.empty()) return !cancel.cancelled();
    const size_t n = static_cast<size_t>(w) * h;
    const TrimGuard trim;
    auto& pool = ThreadPool::shared();
    std::vector<float>&s = plane(0, n), &blur = plane(1, n), &tmp = plane(2, n);
    pool.parallel_for(h, kRowBlock, [&](int y0, int y1) {
        for (size_t i = static_cast<size_t>(y0) * w; i < static_cast<size_t>(y1) * w; ++i)
            s[i] = std::sqrt(lum_of(rgb + i * 3));
    });
    convolve(s.data(), blur.data(), tmp.data(), w, h, kernel, cancel);
    if (cancel.cancelled()) return false;
    const float amount = p.sharpness, th2 = kSharpenThreshold * kSharpenThreshold;
    pool.parallel_for(h, kRowBlock, [&](int y0, int y1) {
        if (cancel.cancelled()) return;
        for (size_t i = static_cast<size_t>(y0) * w; i < static_cast<size_t>(y1) * w; ++i) {
            const float d = s[i] - blur[i];
            // しきい値より小さい細部（ノイズ）はほとんど強めない（なめらかに切り替える）
            const float d2 = d * d;
            set_lum(rgb + i * 3, s[i], s[i] + amount * d * (d2 / (d2 + th2)), true);
        }
    });
    return !cancel.cancelled();
}

} // namespace focal
