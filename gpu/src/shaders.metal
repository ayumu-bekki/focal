// 5.4 章 (1)〜(7) の GPU 版（Metal）。core の CPU 版（renderer.cpp / color_pipeline.cpp / output_transform.cpp）と
// 同じ式・同じ表で計算する。変えるときは両方を変え、tests/test_gpu.cpp で一致を確かめること。
#include <metal_stdlib>
using namespace metal;

// C++ 側の GpuParams と同じ並び（すべて 4 バイト）
struct Params {
    float a, b, c, d, e, f;  // 出力画素座標 → ソース画素座標（output_to_source）
    uint out_w, out_h;
    uint src_w, src_h;
    uint src_full;           // 1: uint16 RGB（フル解像度）、0: float RGB（プロキシ）
    float wb0, wb1, wb2, clip;
    float m[9];              // カメラ RGB → リニア Rec.2020
    float tone_min_x;
    uint tone_n;
    uint color_adjust;
    float saturation, vibrance;
    float om[9];             // リニア Rec.2020 → 出力原色のリニア値
    uint layout;             // 0: RGB、1: BGRX
    // レンズ補正（lens_correction.h の LensMaps）。lens_on = 1 のとき a〜f は 出力 → センサー座標
    uint lens_on, lens_tca, lens_gain, lens_nx, lens_ny;
    float lens_w, lens_h;
    float lens_kx, lens_ky;  // センサー座標 → ソースの画素
};

constant float kMinLog2 = -20.0f;
constant float kMaxLog2 = 6.0f;
constant float kLr = 0.2627f, kLg = 0.6780f, kLb = 0.0593f;
constant float kU16ToFloat = 1.0f / 65535.0f;

inline float3 fetch(device const ushort* s16, device const float* s32, uint full, uint i) {
    if (full != 0) return float3(float(s16[i * 3]), float(s16[i * 3 + 1]), float(s16[i * 3 + 2])) * kU16ToFloat;
    return float3(s32[i * 3], s32[i * 3 + 1], s32[i * 3 + 2]);
}

// renderer.cpp の sample_bilinear / outside と同じ（x, y は画素中心を 0.5 とする連続座標）
inline float3 sample_source(constant Params& p, device const ushort* s16, device const float* s32, float x, float y) {
    const float w = float(p.src_w), h = float(p.src_h);
    if (x < -0.5f || y < -0.5f || x > w + 0.5f || y > h + 0.5f) return float3(0.0f);
    const float fx = x - 0.5f, fy = y - 0.5f;
    int x0 = int(floor(fx)), y0 = int(floor(fy));
    const float tx = fx - float(x0), ty = fy - float(y0);
    const int x1 = clamp(x0 + 1, 0, int(p.src_w) - 1), y1 = clamp(y0 + 1, 0, int(p.src_h) - 1);
    x0 = clamp(x0, 0, int(p.src_w) - 1);
    y0 = clamp(y0, 0, int(p.src_h) - 1);
    const float3 a = fetch(s16, s32, p.src_full, uint(y0) * p.src_w + uint(x0));
    const float3 b = fetch(s16, s32, p.src_full, uint(y0) * p.src_w + uint(x1));
    const float3 d = fetch(s16, s32, p.src_full, uint(y1) * p.src_w + uint(x0));
    const float3 e = fetch(s16, s32, p.src_full, uint(y1) * p.src_w + uint(x1));
    const float3 top = a + (b - a) * tx, bot = d + (e - d) * tx;
    return top + (bot - top) * ty;
}

// renderer.cpp の sample_row_lens と同じ。補正マップ（格子点ごとに 8 要素: R, G, B の座標、ゲイン）を双一次補間で引き、
// R・G・B の座標でソースを引いて、周辺減光のゲインを掛ける
inline float3 sample_source_lens(constant Params& p, device const ushort* s16, device const float* s32,
                                 device const float* lens, float px, float py) {
    const float u = clamp(px / p.lens_w * float(p.lens_nx - 1), 0.0f, float(p.lens_nx - 1));
    const float v = clamp(py / p.lens_h * float(p.lens_ny - 1), 0.0f, float(p.lens_ny - 1));
    const int i0 = min(int(u), int(p.lens_nx) - 2), j0 = min(int(v), int(p.lens_ny) - 2);
    const float tx = u - float(i0), ty = v - float(j0);
    device const float* n00 = lens + (uint(j0) * p.lens_nx + uint(i0)) * 8;
    device const float* n10 = n00 + 8;
    device const float* n01 = n00 + p.lens_nx * 8;
    device const float* n11 = n01 + 8;
    float q[7];
    for (int k = 0; k < 7; ++k) {
        const float top = n00[k] + (n10[k] - n00[k]) * tx;
        const float bot = n01[k] + (n11[k] - n01[k]) * tx;
        q[k] = top + (bot - top) * ty;
    }
    float3 c;
    if (p.lens_tca == 0) {
        c = sample_source(p, s16, s32, q[2] * p.lens_kx, q[3] * p.lens_ky);
    } else {
        c.x = sample_source(p, s16, s32, q[0] * p.lens_kx, q[1] * p.lens_ky).x;
        c.y = sample_source(p, s16, s32, q[2] * p.lens_kx, q[3] * p.lens_ky).y;
        c.z = sample_source(p, s16, s32, q[4] * p.lens_kx, q[5] * p.lens_ky).z;
    }
    return p.lens_gain != 0 ? c * q[6] : c;
}

inline float3 sample_pixel(constant Params& p, device const ushort* s16, device const float* s32,
                           device const float* lens, float x, float y) {
    return p.lens_on != 0 ? sample_source_lens(p, s16, s32, lens, x, y) : sample_source(p, s16, s32, x, y);
}

// tone_curve.cpp の ToneLut::apply と同じ
inline float tone(float x, device const float* lut, float min_x, uint n) {
    if (!(x > min_x)) return x > 0.0f ? lut[0] * (x / min_x) : 0.0f;
    const float pos = (log2(x) - kMinLog2) * (float(n - 1) / (kMaxLog2 - kMinLog2));
    if (pos >= float(n - 1)) return lut[n - 1];
    const int i = int(pos);
    const float f = pos - float(i);
    return lut[i] + (lut[i + 1] - lut[i]) * f;
}

// color_pipeline.cpp の process_tone（(2)〜(5)）
inline float3 process_tone(constant Params& p, device const float* lut, float3 c) {
    const float r = min(c.x * p.wb0, p.clip), g = min(c.y * p.wb1, p.clip), b = min(c.z * p.wb2, p.clip);
    const float x = p.m[0] * r + p.m[1] * g + p.m[2] * b;
    const float y = p.m[3] * r + p.m[4] * g + p.m[5] * b;
    const float z = p.m[6] * r + p.m[7] * g + p.m[8] * b;
    return float3(tone(x, lut, p.tone_min_x, p.tone_n), tone(y, lut, p.tone_min_x, p.tone_n),
                  tone(z, lut, p.tone_min_x, p.tone_n));
}

inline float skin_weight(float r, float g, float b, float mx, float mn) {
    if (mx != r || g < b || mx - mn < 1e-6f) return 0.0f;
    const float h = (g - b) / (mx - mn);
    const float t = 1.0f - abs(h - 0.45f) / 0.3f;
    return clamp(t, 0.0f, 1.0f);
}

// color_pipeline.cpp の process_color（(5b)）
inline float3 process_color(constant Params& p, float3 c) {
    if (p.color_adjust == 0) return c;
    const float r = c.x, g = c.y, b = c.z;
    const float l = kLr * r + kLg * g + kLb * b;
    float k = p.saturation;
    if (p.vibrance != 0.0f) {
        const float mx = max(r, max(g, b)), mn = min(r, min(g, b));
        if (mx > 1e-6f) {
            const float chroma = clamp((mx - mn) / mx, 0.0f, 1.0f);
            float v = p.vibrance * (1.0f - chroma) * (1.0f - chroma);
            if (p.vibrance > 0.0f) v *= 1.0f - 0.6f * skin_weight(r, g, b, mx, mn);
            k *= 1.0f + v;
        }
    }
    return float3(max(0.0f, l + (r - l) * k), max(0.0f, l + (g - l) * k), max(0.0f, l + (b - l) * k));
}

inline int trc_index(float v) {
    v = v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f;
    return int(v * 65535.0f + 0.5f);
}

// (1)〜(7) を 1 パスで。出力は密に並べる（行のバイト数 = 幅 × 画素のバイト数）。ヒストグラムは R, G, B の順
kernel void render_base(constant Params& p [[buffer(0)]], device const ushort* s16 [[buffer(1)]],
                        device const float* s32 [[buffer(2)]], device const float* lut [[buffer(3)]],
                        device const uchar* trc [[buffer(4)]], device uchar* out [[buffer(5)]],
                        device atomic_uint* hist [[buffer(6)]], device const float* lens [[buffer(7)]], uint2 gid [[thread_position_in_grid]],
                        uint lid [[thread_index_in_threadgroup]], uint2 tsize [[threads_per_threadgroup]]) {
    threadgroup atomic_uint local[768];
    const uint nthreads = tsize.x * tsize.y;
    for (uint i = lid; i < 768; i += nthreads) atomic_store_explicit(&local[i], 0u, memory_order_relaxed);
    threadgroup_barrier(mem_flags::mem_threadgroup);

    const float ox = float(gid.x) + 0.5f, oy = float(gid.y) + 0.5f;
    const float sx = p.a * ox + p.b * oy + p.c, sy = p.d * ox + p.e * oy + p.f;
    float3 c = process_tone(p, lut, sample_pixel(p, s16, s32, lens, sx, sy));
    c = process_color(p, c);
    const uchar r = trc[trc_index(p.om[0] * c.x + p.om[1] * c.y + p.om[2] * c.z)];
    const uchar g = trc[trc_index(p.om[3] * c.x + p.om[4] * c.y + p.om[5] * c.z)];
    const uchar b = trc[trc_index(p.om[6] * c.x + p.om[7] * c.y + p.om[8] * c.z)];
    const uint idx = gid.y * p.out_w + gid.x;
    if (p.layout == 1) {
        out[idx * 4] = b;
        out[idx * 4 + 1] = g;
        out[idx * 4 + 2] = r;
        out[idx * 4 + 3] = 255;
    } else {
        out[idx * 3] = r;
        out[idx * 3 + 1] = g;
        out[idx * 3 + 2] = b;
    }
    atomic_fetch_add_explicit(&local[r], 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&local[256 + g], 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&local[512 + b], 1u, memory_order_relaxed);

    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint i = lid; i < 768; i += nthreads) {
        const uint v = atomic_load_explicit(&local[i], memory_order_relaxed);
        if (v != 0) atomic_fetch_add_explicit(&hist[i], v, memory_order_relaxed);
    }
}

// ---- 周辺画素を使う処理（5.4 章 (5a)、v3.14 G2）--------------------------------------------------------------
// core の detail.cpp / local_contrast.cpp と同じ順番・同じ式で計算する。作業用の平面はすべて float。

// C++ 側の AuxParams と同じ並び（すべて 4 バイト）
struct Aux {
    int w, h;                 // 平面の大きさ
    int r;                    // 窓・カーネルの半径
    int f, gx0, gy0, sw, sh;  // 縮小の区切り（縮小率、縮小画像の左上の区切り番号、縮小画像の大きさ）
    int ox, oy;               // バッファの左上の、出力画像（この縮尺）での位置
    float amount, eps, th2;
    int left, top, bw;        // 仕上げ: 余白込みのバッファの中の描く範囲
};

// 作業用の画像（wk）は半精度（16-bit float）で持つ（メモリの読み書きを減らす。値は 0〜1 なので 8-bit の出力には十分）
inline float lum3(device const half* p) { return kLr * float(p[0]) + kLg * float(p[1]) + kLb * float(p[2]); }

// detail.cpp の set_lum
inline void set_lum(device half* p, float old_sqrt, float new_sqrt, bool cap) {
    const float old_l = old_sqrt * old_sqrt;
    const float ns = max(0.0f, new_sqrt);
    float ratio = old_l > 1e-12f ? ns * ns / max(old_l, 1e-12f) : 1.0f;
    if (cap) {
        const float mx = max(float(p[0]), max(float(p[1]), float(p[2])));
        ratio = min(ratio, max(1.0f, 1.0f / max(mx, 1e-6f)));
    }
    p[0] = half(float(p[0]) * ratio);
    p[1] = half(float(p[1]) * ratio);
    p[2] = half(float(p[2]) * ratio);
}

// (1)〜(5) だけ（余白込みの範囲）。出力は float RGB
kernel void render_tone(constant Params& p [[buffer(0)]], device const ushort* s16 [[buffer(1)]],
                        device const float* s32 [[buffer(2)]], device const float* lut [[buffer(3)]],
                        device half* wk [[buffer(4)]], device const float* lens [[buffer(5)]],
                        uint2 gid [[thread_position_in_grid]]) {
    const float ox = float(gid.x) + 0.5f, oy = float(gid.y) + 0.5f;
    const float sx = p.a * ox + p.b * oy + p.c, sy = p.d * ox + p.e * oy + p.f;
    const float3 c = process_tone(p, lut, sample_pixel(p, s16, s32, lens, sx, sy));
    const uint i = (gid.y * p.out_w + gid.x) * 3;
    wk[i] = half(c.x);
    wk[i + 1] = half(c.y);
    wk[i + 2] = half(c.z);
}

// 箱型の平均（半径 r、端は端の値が続くとみなす）。横と縦に分ける
kernel void box_h(constant Aux& a [[buffer(0)]], device const float* src [[buffer(1)]], device float* dst [[buffer(2)]],
                  uint2 g [[thread_position_in_grid]]) {
    const int y = int(g.y), x = int(g.x);
    float sum = 0;
    for (int j = -a.r; j <= a.r; ++j) sum += src[y * a.w + clamp(x + j, 0, a.w - 1)];
    dst[y * a.w + x] = sum * (1.0f / float(2 * a.r + 1));
}
kernel void box_v(constant Aux& a [[buffer(0)]], device const float* src [[buffer(1)]], device float* dst [[buffer(2)]],
                  uint2 g [[thread_position_in_grid]]) {
    const int y = int(g.y), x = int(g.x);
    float sum = 0;
    for (int j = -a.r; j <= a.r; ++j) sum += src[clamp(y + j, 0, a.h - 1) * a.w + x];
    dst[y * a.w + x] = sum * (1.0f / float(2 * a.r + 1));
}

// カラーのノイズ低減: 縮小（面積平均）で 6 つの平面（ガイド、ガイドの二乗、2 つの色、色 × ガイド）
kernel void cnr_down(constant Aux& a [[buffer(0)]], device const half* wk [[buffer(1)]], device float* lg [[buffer(2)]],
                     device float* lgg [[buffer(3)]], device float* lc0 [[buffer(4)]], device float* lc1 [[buffer(5)]],
                     device float* lg0 [[buffer(6)]], device float* lg1 [[buffer(7)]], uint2 g [[thread_position_in_grid]]) {
    const int sx = int(g.x), sy = int(g.y);
    const int ya = max(0, (a.gy0 + sy) * a.f - a.oy), yb = min(a.h, (a.gy0 + sy + 1) * a.f - a.oy);
    const int xa = max(0, (a.gx0 + sx) * a.f - a.ox), xb = min(a.w, (a.gx0 + sx + 1) * a.f - a.ox);
    float s_g = 0, s_gg = 0, s_c0 = 0, s_c1 = 0, s_g0 = 0, s_g1 = 0;
    for (int y = ya; y < yb; ++y)
        for (int x = xa; x < xb; ++x) {
            device const half* q = wk + (y * a.w + x) * 3;
            const float yl = lum3(q);
            const float gv = sqrt(max(0.0f, yl)), c0 = float(q[0]) - yl, c1 = float(q[2]) - yl;
            s_g += gv;
            s_gg += gv * gv;
            s_c0 += c0;
            s_c1 += c1;
            s_g0 += c0 * gv;
            s_g1 += c1 * gv;
        }
    const float inv = 1.0f / float((yb - ya) * (xb - xa));
    const int j = sy * a.sw + sx;
    lg[j] = s_g * inv;
    lgg[j] = s_gg * inv;
    lc0[j] = s_c0 * inv;
    lc1[j] = s_c1 * inv;
    lg0[j] = s_g0 * inv;
    lg1[j] = s_g1 * inv;
}
// 係数: m0, m1 ← a（R − Y、B − Y）、mg0, mg1 ← b
kernel void cnr_coef(constant Aux& a [[buffer(0)]], device const float* mg [[buffer(1)]], device const float* mgg [[buffer(2)]],
                     device float* m0 [[buffer(3)]], device float* m1 [[buffer(4)]], device float* mg0 [[buffer(5)]],
                     device float* mg1 [[buffer(6)]], uint2 g [[thread_position_in_grid]]) {
    const uint j = g.y * uint(a.w) + g.x;
    const float var = max(0.0f, mgg[j] - mg[j] * mg[j]) + a.eps;
    const float a0 = (mg0[j] - mg[j] * m0[j]) / var, a1 = (mg1[j] - mg[j] * m1[j]) / var;
    const float b0 = m0[j] - a0 * mg[j], b1 = m1[j] - a1 * mg[j];
    m0[j] = a0;
    m1[j] = a1;
    mg0[j] = b0;
    mg1[j] = b1;
}

// 縮小画像の係数を双線形で戻す（横に補間してから縦に補間する。CPU 版と同じ順）
inline float upsample(device const float* m, int sw, int x0, int x1, float tx, int r0, int r1, float ty) {
    const float t = m[r0 * sw + x0] + (m[r0 * sw + x1] - m[r0 * sw + x0]) * tx;
    const float b = m[r1 * sw + x0] + (m[r1 * sw + x1] - m[r1 * sw + x0]) * tx;
    return t + (b - t) * ty;
}
struct Upsample {
    int x0, x1, r0, r1;
    float tx, ty;
};
inline Upsample upsample_pos(constant Aux& a, int x, int y) {
    const float inv_f = 1.0f / float(a.f);
    const float fx = clamp((float(a.ox + x) + 0.5f) * inv_f - 0.5f - float(a.gx0), 0.0f, float(a.sw - 1));
    const float fy = clamp((float(a.oy + y) + 0.5f) * inv_f - 0.5f - float(a.gy0), 0.0f, float(a.sh - 1));
    Upsample u;
    u.x0 = int(fx);
    u.x1 = min(u.x0 + 1, a.sw - 1);
    u.tx = fx - float(u.x0);
    u.r0 = int(fy);
    u.r1 = min(u.r0 + 1, a.sh - 1);
    u.ty = fy - float(u.r0);
    return u;
}
kernel void cnr_apply(constant Aux& a [[buffer(0)]], device half* wk [[buffer(1)]], device const float* m0 [[buffer(2)]],
                      device const float* mg0 [[buffer(3)]], device const float* m1 [[buffer(4)]],
                      device const float* mg1 [[buffer(5)]], uint2 g [[thread_position_in_grid]]) {
    const int x = int(g.x), y = int(g.y);
    const Upsample u = upsample_pos(a, x, y);
    const float a0 = upsample(m0, a.sw, u.x0, u.x1, u.tx, u.r0, u.r1, u.ty);
    const float b0 = upsample(mg0, a.sw, u.x0, u.x1, u.tx, u.r0, u.r1, u.ty);
    const float a1 = upsample(m1, a.sw, u.x0, u.x1, u.tx, u.r0, u.r1, u.ty);
    const float b1 = upsample(mg1, a.sw, u.x0, u.x1, u.tx, u.r0, u.r1, u.ty);
    device half* q = wk + (y * a.w + x) * 3;
    const float yy = lum3(q);
    const float gv = sqrt(max(0.0f, yy));
    const float r = max(0.0f, yy + a0 * gv + b0), b = max(0.0f, yy + a1 * gv + b1);
    q[0] = half(r);
    q[2] = half(b);
    q[1] = half(max(0.0f, (yy - kLr * r - kLb * b) / kLg));
}

// 明瞭度（local_contrast.cpp）
constant float kMinLum = 1e-5f;
constant float kMidLog = -2.4739f;
constant float kMidWidth = 2.0f;
constant float kEdgeEps = 0.1f;
constant float kStrength = 1.5f;

// 係数: a ← k、b ← a × (1 − k)（入力 a は平均、ii は二乗の平均）
kernel void lc_coef(constant Aux& a [[buffer(0)]], device float* ca [[buffer(1)]], device const float* ii [[buffer(2)]],
                    device float* cb [[buffer(3)]], uint2 g [[thread_position_in_grid]]) {
    const uint i = g.y * uint(a.w) + g.x;
    const float var = max(0.0f, ii[i] - ca[i] * ca[i]);
    const float k = var / (var + kEdgeEps);
    cb[i] = ca[i] * (1.0f - k);
    ca[i] = k;
}
inline float fast_exp2(float x) {
    const float xi = floor(x);
    const float f = x - xi;
    const float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * 0.0096181f)));
    const int e = clamp(int(xi), -126, 127) + 127;
    return p * as_type<float>(uint(e) << 23);
}

// 仕上げ: 余白込みのバッファから描く範囲を取り出し、(5b)〜(7) とヒストグラム
kernel void finish(constant Params& p [[buffer(0)]], constant Aux& a [[buffer(1)]], device const half* wk [[buffer(2)]],
                   device const uchar* trc [[buffer(3)]], device uchar* out [[buffer(4)]],
                   device atomic_uint* hist [[buffer(5)]], uint2 gid [[thread_position_in_grid]],
                   uint lid [[thread_index_in_threadgroup]], uint2 tsize [[threads_per_threadgroup]]) {
    threadgroup atomic_uint local[768];
    const uint nthreads = tsize.x * tsize.y;
    for (uint i = lid; i < 768; i += nthreads) atomic_store_explicit(&local[i], 0u, memory_order_relaxed);
    threadgroup_barrier(mem_flags::mem_threadgroup);

    device const half* q = wk + ((int(gid.y) + a.top) * a.bw + int(gid.x) + a.left) * 3;
    const float3 c = process_color(p, float3(float(q[0]), float(q[1]), float(q[2])));
    const uchar r = trc[trc_index(p.om[0] * c.x + p.om[1] * c.y + p.om[2] * c.z)];
    const uchar g = trc[trc_index(p.om[3] * c.x + p.om[4] * c.y + p.om[5] * c.z)];
    const uchar b = trc[trc_index(p.om[6] * c.x + p.om[7] * c.y + p.om[8] * c.z)];
    const uint idx = gid.y * p.out_w + gid.x;
    if (p.layout == 1) {
        out[idx * 4] = b;
        out[idx * 4 + 1] = g;
        out[idx * 4 + 2] = r;
        out[idx * 4 + 3] = 255;
    } else {
        out[idx * 3] = r;
        out[idx * 3 + 1] = g;
        out[idx * 3 + 2] = b;
    }
    atomic_fetch_add_explicit(&local[r], 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&local[256 + g], 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&local[512 + b], 1u, memory_order_relaxed);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint i = lid; i < 768; i += nthreads) {
        const uint v = atomic_load_explicit(&local[i], memory_order_relaxed);
        if (v != 0) atomic_fetch_add_explicit(&hist[i], v, memory_order_relaxed);
    }
}

// ---- タイルで 1 回にまとめた版（メモリの読み書きを減らす。v3.14）------------------------------------------------
// スレッドグループ（16 × 16）ごとに、周りの余白（半径 R 以下）込みのタイルを共有メモリに読み込んで計算する。
// 箱型の平均は 2 次元の和を直接取る（CPU 版の「横 → 縦」と丸めの差だけ違う）
constant int kTile = 16;
constant int kMaxHalo = 8;
constant int kTileMax = kTile + 2 * kMaxHalo;  // 32

// タイル（左上 = タイルの原点 − halo）を読み込む。端は端の値が続くとみなす
inline void load_tile(threadgroup float* t, device const float* src, int w, int h, int2 origin, int halo, uint lid,
                      uint nthreads) {
    const int span = kTile + 2 * halo;
    for (uint i = lid; i < uint(span * span); i += nthreads) {
        const int ty = int(i) / span, tx = int(i) % span;
        const int x = clamp(origin.x - halo + tx, 0, w - 1), y = clamp(origin.y - halo + ty, 0, h - 1);
        t[ty * kTileMax + tx] = src[y * w + x];
    }
}

// 輝度のノイズ低減 1: s（√輝度）の窓の平均と二乗の平均から、係数 a, b
kernel void nr_tile1(constant Aux& a [[buffer(0)]], device const float* s [[buffer(1)]], device float* ca [[buffer(2)]],
                     device float* cb [[buffer(3)]], uint2 gid [[thread_position_in_grid]],
                     uint2 tg [[threadgroup_position_in_grid]], uint2 tid [[thread_position_in_threadgroup]],
                     uint lid [[thread_index_in_threadgroup]], uint2 tsize [[threads_per_threadgroup]]) {
    threadgroup float t[kTileMax * kTileMax];
    load_tile(t, s, a.w, a.h, int2(tg) * kTile, a.r, lid, tsize.x * tsize.y);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float sum = 0, sum2 = 0;
    for (int dy = 0; dy <= 2 * a.r; ++dy)
        for (int dx = 0; dx <= 2 * a.r; ++dx) {
            const float v = t[(int(tid.y) + dy) * kTileMax + int(tid.x) + dx];
            sum += v;
            sum2 += v * v;
        }
    const float inv = 1.0f / float((2 * a.r + 1) * (2 * a.r + 1));
    const float mean = sum * inv;
    const float var = max(0.0f, sum2 * inv - mean * mean);
    const float k = var / (var + a.eps);
    const uint i = gid.y * uint(a.w) + gid.x;
    ca[i] = k;
    cb[i] = mean * (1.0f - k);
}

// 輝度のノイズ低減 2: 係数の窓の平均を取り、輝度を a × s + b にする
kernel void nr_tile2(constant Aux& a [[buffer(0)]], device half* wk [[buffer(1)]], device const float* s [[buffer(2)]],
                     device const float* ca [[buffer(3)]], device const float* cb [[buffer(4)]],
                     uint2 gid [[thread_position_in_grid]], uint2 tg [[threadgroup_position_in_grid]],
                     uint2 tid [[thread_position_in_threadgroup]], uint lid [[thread_index_in_threadgroup]],
                     uint2 tsize [[threads_per_threadgroup]]) {
    threadgroup float ta[kTileMax * kTileMax];
    threadgroup float tb[kTileMax * kTileMax];
    load_tile(ta, ca, a.w, a.h, int2(tg) * kTile, a.r, lid, tsize.x * tsize.y);
    load_tile(tb, cb, a.w, a.h, int2(tg) * kTile, a.r, lid, tsize.x * tsize.y);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float sa = 0, sb = 0;
    for (int dy = 0; dy <= 2 * a.r; ++dy)
        for (int dx = 0; dx <= 2 * a.r; ++dx) {
            const int j = (int(tid.y) + dy) * kTileMax + int(tid.x) + dx;
            sa += ta[j];
            sb += tb[j];
        }
    const float inv = 1.0f / float((2 * a.r + 1) * (2 * a.r + 1));
    const uint i = gid.y * uint(a.w) + gid.x;
    set_lum(wk + i * 3, s[i], sa * inv * s[i] + sb * inv, false);
}

// シャープネス: s のガウスぼかし（横 → 縦、CPU 版と同じ順で足す）と適用を 1 回で
kernel void sh_tile(constant Aux& a [[buffer(0)]], device half* wk [[buffer(1)]], device const float* s [[buffer(2)]],
                    device const float* k [[buffer(3)]], uint2 gid [[thread_position_in_grid]],
                    uint2 tg [[threadgroup_position_in_grid]], uint2 tid [[thread_position_in_threadgroup]],
                    uint lid [[thread_index_in_threadgroup]], uint2 tsize [[threads_per_threadgroup]]) {
    threadgroup float t[kTileMax * kTileMax];
    threadgroup float hz[kTileMax * kTile];  // 横にぼかした値（余白込みの行 × タイルの列）
    const uint nthreads = tsize.x * tsize.y;
    load_tile(t, s, a.w, a.h, int2(tg) * kTile, a.r, lid, nthreads);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    const int rows = kTile + 2 * a.r;
    for (uint i = lid; i < uint(rows * kTile); i += nthreads) {
        const int ty = int(i) / kTile, tx = int(i) % kTile;
        float acc = 0;
        for (int j = 0; j <= 2 * a.r; ++j) acc += k[j] * t[ty * kTileMax + tx + j];
        hz[ty * kTile + tx] = acc;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float blur = 0;
    for (int j = 0; j <= 2 * a.r; ++j) blur += k[j] * hz[(int(tid.y) + j) * kTile + int(tid.x)];
    const uint i = gid.y * uint(a.w) + gid.x;
    const float sv = s[i];
    const float d = sv - blur;
    const float d2 = d * d;
    set_lum(wk + i * 3, sv, sv + a.amount * d * (d2 / (d2 + a.th2)), true);
}

// 明瞭度: 輝度の対数の平面を作らず、wk から直接縮小する
kernel void lc_down_wk(constant Aux& a [[buffer(0)]], device const half* wk [[buffer(1)]], device float* small [[buffer(2)]],
                       device float* ii [[buffer(3)]], uint2 g [[thread_position_in_grid]]) {
    const int sx = int(g.x), sy = int(g.y);
    const int ya = max(0, (a.gy0 + sy) * a.f - a.oy), yb = min(a.h, (a.gy0 + sy + 1) * a.f - a.oy);
    const int xa = max(0, (a.gx0 + sx) * a.f - a.ox), xb = min(a.w, (a.gx0 + sx + 1) * a.f - a.ox);
    float sum = 0;
    for (int y = ya; y < yb; ++y)
        for (int x = xa; x < xb; ++x) sum += log2(max(lum3(wk + (y * a.w + x) * 3), kMinLum));
    const float m = sum / float((yb - ya) * (xb - xa));
    small[sy * a.sw + sx] = m;
    ii[sy * a.sw + sx] = m * m;
}
kernel void lc_apply_wk(constant Aux& a [[buffer(0)]], device half* wk [[buffer(1)]], device const float* ca [[buffer(2)]],
                        device const float* cb [[buffer(3)]], uint2 g [[thread_position_in_grid]]) {
    const int x = int(g.x), y = int(g.y);
    const Upsample u = upsample_pos(a, x, y);
    const float A = upsample(ca, a.sw, u.x0, u.x1, u.tx, u.r0, u.r1, u.ty);
    const float B = upsample(cb, a.sw, u.x0, u.x1, u.tx, u.r0, u.r1, u.ty);
    device half* p = wk + (y * a.w + x) * 3;
    const float l = log2(max(lum3(p), kMinLum));
    const float d = l - (A * l + B);
    const float t = (l - kMidLog) * (1.0f / kMidWidth);
    const float t2 = t * t;
    const float wmid = 1.0f / (1.0f + t2 * t2);
    const float e = a.amount > 0.0f ? kStrength * a.amount * wmid * d / (1.0f + abs(d)) : 0.5f * a.amount * wmid * d;
    float k = fast_exp2(e);
    if (k > 1.0f) {
        const float mx = max(float(p[0]), max(float(p[1]), float(p[2])));
        if (mx * k > 1.0f) k = max(1.0f, 1.0f / mx);
    }
    p[0] = half(float(p[0]) * k);
    p[1] = half(float(p[1]) * k);
    p[2] = half(float(p[2]) * k);
}

// √輝度だけ
kernel void sqrt_lum1(constant Aux& a [[buffer(0)]], device const half* wk [[buffer(1)]], device float* s [[buffer(2)]],
                      uint2 g [[thread_position_in_grid]]) {
    const uint i = g.y * uint(a.w) + g.x;
    s[i] = sqrt(max(0.0f, lum3(wk + i * 3)));
}
