#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "imaging/color_pipeline.h"
#include "imaging/detail.h"
#include "imaging/renderer.h"

using namespace focal;
using Catch::Approx;

namespace {

constexpr int W = 400, H = 200;

// 中間グレー。左半分は平ら、右半分は振幅 25% の縞（細部）。輝度ノイズ（相対 sigma）とカラーノイズを足せる
struct Scene {
    std::vector<float> img, clean;
    Scene(float luma_noise, float chroma_noise = 0) : img(static_cast<size_t>(W) * H * 3), clean(img.size()) {
        std::mt19937 rng(7);
        std::normal_distribution<float> nd(0, 1);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                float g = 0.18f;
                if (x >= W / 2) g *= 1.0f + 0.25f * ((x / 3) % 2 ? 1 : -1);
                const float n = g * (1 + luma_noise * nd(rng));
                const float cr = g * chroma_noise * nd(rng), cb = g * chroma_noise * nd(rng);
                const size_t i = (static_cast<size_t>(y) * W + x) * 3;
                clean[i] = clean[i + 1] = clean[i + 2] = g;
                img[i] = n + cr;
                img[i + 2] = n + cb;
                img[i + 1] = n - (0.2627f * cr + 0.0593f * cb) / 0.6780f;  // 輝度は n のまま
            }
    }
    // 平らな部分の、きれいな画像との差（G）
    double flat_error(const std::vector<float>& v) const {
        double e = 0;
        int k = 0;
        for (int y = 20; y < H - 20; ++y)
            for (int x = 20; x < W / 2 - 20; ++x, ++k) {
                const size_t i = (static_cast<size_t>(y) * W + x) * 3 + 1;
                e += (v[i] - clean[i]) * (v[i] - clean[i]);
            }
        return std::sqrt(e / k);
    }
    // 縞の振幅（元を 1 とする）
    double stripe(const std::vector<float>& v) const {
        double a = 0;
        int k = 0;
        for (int y = 20; y < H - 20; ++y)
            for (int x = W / 2 + 20; x < W - 20; ++x, ++k) a += std::abs(v[(static_cast<size_t>(y) * W + x) * 3 + 1] - 0.18);
        return a / k / (0.18 * 0.25);
    }
};

double chroma_error(const std::vector<float>& v) {
    double e = 0;
    int k = 0;
    for (int y = 20; y < H - 20; ++y)
        for (int x = 20; x < W / 2 - 20; ++x, ++k) {
            const size_t i = (static_cast<size_t>(y) * W + x) * 3;
            e += (v[i] - v[i + 1]) * (v[i] - v[i + 1]) + (v[i + 2] - v[i + 1]) * (v[i + 2] - v[i + 1]);
        }
    return std::sqrt(e / k);
}

ColorInfo srgb_camera() {
    ColorInfo c;
    c.rgb_cam = Mat3::identity();
    c.cam_xyz = rgb_to_xyz_matrix(primaries::kSrgb, primaries::kD65).inverse();
    return c;
}

} // namespace

TEST_CASE("ディテール: 既定値では何もしない", "[detail]") {
    const DetailParams p;
    CHECK_FALSE(p.any());
    Scene s(0.04f, 0.03f);
    auto v = s.img;
    REQUIRE(apply_noise_reduction(v.data(), W, H, p, 1.0));
    REQUIRE(apply_sharpen(v.data(), W, H, p, 1.0));
    CHECK(v == s.img);
    CHECK(detail_margin(p, 1.0) == 0);
}

TEST_CASE("ノイズ低減（輝度）: 平らな面のノイズを減らし、細部（縞）は残す", "[detail]") {
    Scene s(0.04f);
    DetailParams p;
    // 量 50: ノイズ約 1/4、細部 9 割。量 100: ノイズ約 1/8、細部 7 割
    p.noise_reduction = 0.5f;
    auto half = s.img;
    REQUIRE(apply_noise_reduction(half.data(), W, H, p, 1.0));
    CHECK(s.flat_error(half) < s.flat_error(s.img) * 0.35);
    CHECK(s.stripe(half) > 0.85);
    p.noise_reduction = 1.0f;
    auto full = s.img;
    REQUIRE(apply_noise_reduction(full.data(), W, H, p, 1.0));
    CHECK(s.flat_error(full) < s.flat_error(s.img) * 0.2);
    CHECK(s.stripe(full) > 0.65);
}

TEST_CASE("ノイズ低減（カラー）: 色のまだらを減らし、輝度は変えない", "[detail]") {
    Scene s(0.0f, 0.04f);
    DetailParams p;
    p.color_noise_reduction = 1.0f;
    auto v = s.img;
    REQUIRE(apply_noise_reduction(v.data(), W, H, p, 1.0));
    CHECK(chroma_error(v) < chroma_error(s.img) * 0.3);
    for (size_t i = 0; i < v.size(); i += 3) {
        const float l0 = 0.2627f * s.img[i] + 0.6780f * s.img[i + 1] + 0.0593f * s.img[i + 2];
        const float l1 = 0.2627f * v[i] + 0.6780f * v[i + 1] + 0.0593f * v[i + 2];
        CHECK(l1 == Approx(l0).margin(2e-3));
    }
}

TEST_CASE("ノイズ低減（カラー）: 輝度の輪郭に沿った色の境界は保つ", "[detail]") {
    // 左は暗い赤、右は明るい緑（色の境界と輝度の境界が同じ位置）
    std::vector<float> v(static_cast<size_t>(W) * H * 3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float* q = &v[(static_cast<size_t>(y) * W + x) * 3];
            if (x < W / 2) {
                q[0] = 0.10f, q[1] = 0.03f, q[2] = 0.03f;
            } else {
                q[0] = 0.15f, q[1] = 0.45f, q[2] = 0.15f;
            }
        }
    const auto before = v;
    DetailParams p;
    p.color_noise_reduction = 1.0f;
    REQUIRE(apply_noise_reduction(v.data(), W, H, p, 1.0));
    // 境界の両隣（1 画素）の色はほとんど変わらない（単純なぼかしだと境界の 8 画素ほどが混ざる）
    for (int x : {W / 2 - 1, W / 2}) {
        const size_t i = (static_cast<size_t>(H / 2) * W + x) * 3;
        for (int c = 0; c < 3; ++c) CHECK(v[i + c] == Approx(before[i + c]).margin(0.02));
    }
}

TEST_CASE("シャープネス: 細部を強め、しきい値より小さいノイズはほとんど強めない", "[detail]") {
    Scene s(0.002f);  // ごく小さなノイズ
    DetailParams p;
    p.sharpness = 1.5f;
    auto v = s.img;
    REQUIRE(apply_sharpen(v.data(), W, H, p, 1.0));
    CHECK(s.stripe(v) > 1.2);
    CHECK(s.flat_error(v) < s.flat_error(s.img) * 1.3);
    for (float x : v) CHECK((std::isfinite(x) && x >= 0.0f && x <= 1.0f + 1e-6f));
}

TEST_CASE("ディテール: 縮小表示（半径が 1 画素未満）では行わない", "[detail]") {
    Scene s(0.04f, 0.04f);
    DetailParams p;
    p.noise_reduction = 1.0f;
    p.sharpness = 1.0f;
    p.color_noise_reduction = 0.04f;  // σ = 4 × 0.04 × 0.1 < 0.25
    auto v = s.img;
    REQUIRE(apply_noise_reduction(v.data(), W, H, p, 0.1));
    REQUIRE(apply_sharpen(v.data(), W, H, p, 0.1));
    CHECK(v == s.img);
}

TEST_CASE("ディテール: 一部の範囲を描いても全体を描いたときと同じ値になる（100% 表示）", "[detail][pipeline]") {
    const int sw = 600, sh = 400;
    ImageF proxy(sw, sh);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> u(0.05f, 0.5f);
    for (auto& x : proxy.data) x = u(rng);
    Settings st;
    st.noise_reduction = 60;
    st.color_noise_reduction = 50;
    st.sharpness = 100;
    st.clarity = 40;
    const ColorPipeline pipeline(st, srgb_camera());
    const GeometryPlan plan(sw, sh, 0, st.geometry);
    const SourceView src{nullptr, &proxy, sw, sh};
    std::vector<float> whole(static_cast<size_t>(sw) * sh * 3);
    REQUIRE(render_linear(src, plan, 1.0, {}, sw, sh, pipeline, Interpolation::Bilinear, whole.data()));
    const int rx = 211, ry = 137, rw = 150, rh = 120;
    std::vector<float> part(static_cast<size_t>(rw) * rh * 3);
    REQUIRE(render_linear(src, plan, 1.0, {static_cast<double>(rx), static_cast<double>(ry)}, rw, rh, pipeline,
                          Interpolation::Bilinear, part.data()));
    float max_diff = 0;
    for (int y = 0; y < rh; ++y)
        for (int x = 0; x < rw * 3; ++x)
            max_diff = std::max(max_diff, std::abs(part[static_cast<size_t>(y) * rw * 3 + x] -
                                                   whole[(static_cast<size_t>(y + ry) * sw + rx) * 3 + x]));
    CHECK(max_diff < 2e-3f);
}

TEST_CASE("ノイズ低減（カラー）: RGB 各色に別々に乗る大きな色むらも減らす", "[detail]") {
    // 実際のセンサーのように RGB それぞれに独立のノイズ（σ 6 画素の大きな粒、信号の 6%）を乗せた平らなグレー。
    // 色の揺れが輝度の揺れと相関するので、輝度をガイドにするときに取り違えやすい（v3.13 の最初の版は 100 でも 0.6 倍）
    const int w = 600, h = 300;
    std::mt19937 rng(5);
    std::normal_distribution<float> nd(0, 1);
    std::vector<float> img(static_cast<size_t>(w) * h * 3);
    for (int c = 0; c < 3; ++c) {
        // 白色ノイズを σ 6 でぼかし、分散を 1 に戻す
        std::vector<float> f(static_cast<size_t>(w) * h), t(f.size());
        for (auto& v : f) v = nd(rng);
        const int r = 18;
        std::vector<float> k(2 * r + 1);
        double s = 0, e = 0;
        for (int i = -r; i <= r; ++i) s += k[static_cast<size_t>(i + r)] = static_cast<float>(std::exp(-i * i / 72.0));
        for (auto& v : k) {
            v = static_cast<float>(v / s);
            e += v * v;
        }
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float a = 0;
                for (int j = -r; j <= r; ++j) a += k[static_cast<size_t>(j + r)] * f[static_cast<size_t>(y) * w + std::clamp(x + j, 0, w - 1)];
                t[static_cast<size_t>(y) * w + x] = a;
            }
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float a = 0;
                for (int j = -r; j <= r; ++j) a += k[static_cast<size_t>(j + r)] * t[static_cast<size_t>(std::clamp(y + j, 0, h - 1)) * w + x];
                img[(static_cast<size_t>(y) * w + x) * 3 + c] = 0.18f * (1 + 0.06f * a / static_cast<float>(e));
            }
    }
    auto chroma = [&](const std::vector<float>& v) {
        double sum = 0;
        int n = 0;
        for (int y = 60; y < h - 60; ++y)
            for (int x = 60; x < w - 60; ++x, ++n) {
                const size_t i = (static_cast<size_t>(y) * w + x) * 3;
                sum += (v[i] - v[i + 1]) * (v[i] - v[i + 1]) + (v[i + 2] - v[i + 1]) * (v[i + 2] - v[i + 1]);
            }
        return std::sqrt(sum / n);
    };
    DetailParams p;
    p.color_noise_reduction = 0.5f;
    auto half = img;
    REQUIRE(apply_noise_reduction(half.data(), w, h, p, 1.0));
    p.color_noise_reduction = 1.0f;
    auto full = img;
    REQUIRE(apply_noise_reduction(full.data(), w, h, p, 1.0));
    CHECK(chroma(half) < chroma(img) * 0.45);
    CHECK(chroma(full) < chroma(img) * 0.25);
}
