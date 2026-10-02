#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "imaging/color_pipeline.h"
#include "imaging/local_contrast.h"
#include "imaging/renderer.h"

using namespace focal;
using Catch::Approx;

namespace {

// 中間グレーの上に細かい明暗（質感）を乗せたグレー画像
std::vector<float> texture(int w, int h, float base = 0.18f, float amp = 0.25f) {
    std::vector<float> v(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float g = base * std::exp2(amp * std::sin(x * 0.7f) * std::sin(y * 0.5f));
            for (int c = 0; c < 3; ++c) v[(static_cast<size_t>(y) * w + x) * 3 + c] = g;
        }
    return v;
}

// G チャンネルの対数の標準偏差（明暗の強さ）
double log_spread(const std::vector<float>& v, int w, int x0, int y0, int x1, int y1) {
    double s = 0, s2 = 0;
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x, ++n) {
            const double l = std::log2(v[(static_cast<size_t>(y) * w + x) * 3 + 1]);
            s += l;
            s2 += l * l;
        }
    return std::sqrt(s2 / n - (s / n) * (s / n));
}

ColorInfo srgb_camera() {
    ColorInfo c;
    c.rgb_cam = Mat3::identity();
    c.cam_xyz = rgb_to_xyz_matrix(primaries::kSrgb, primaries::kD65).inverse();
    return c;
}

} // namespace

TEST_CASE("明瞭度: 0 なら何もしない", "[clarity]") {
    auto v = texture(200, 150);
    const auto before = v;
    REQUIRE(apply_local_contrast(v.data(), 200, 150, 0.0f, 20.0));
    CHECK(v == before);
}

TEST_CASE("明瞭度: 正で細かい明暗が強まり、負で弱まる。グレーは色付かない", "[clarity]") {
    const int w = 300, h = 200;
    const auto base = texture(w, h);
    const double s0 = log_spread(base, w, 50, 50, 250, 150);
    auto up = base, down = base;
    REQUIRE(apply_local_contrast(up.data(), w, h, 1.0f, 20.0));
    REQUIRE(apply_local_contrast(down.data(), w, h, -1.0f, 20.0));
    CHECK(log_spread(up, w, 50, 50, 250, 150) > s0 * 1.1);
    CHECK(log_spread(down, w, 50, 50, 250, 150) < s0 * 0.9);
    for (size_t i = 0; i < up.size(); i += 3) {
        CHECK(up[i] == Approx(up[i + 1]).epsilon(1e-6));
        CHECK(up[i + 2] == Approx(up[i + 1]).epsilon(1e-6));
    }
}

TEST_CASE("明瞭度: 大きな明暗の境界の近くの平らな面にハローを作らない", "[clarity]") {
    // 左半分 0.03、右半分 0.5 の平らな画像（質感なし）
    const int w = 400, h = 100;
    std::vector<float> v(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) v[(static_cast<size_t>(y) * w + x) * 3 + c] = x < w / 2 ? 0.03f : 0.5f;
    const auto before = v;
    REQUIRE(apply_local_contrast(v.data(), w, h, 1.0f, 20.0));
    // 境界から σ 離れた画素の変化は 4% 以内（エッジを保つぼかしなので、境界の両側に明暗の縁取りを作らない）
    for (int x : {w / 2 - 20, w / 2 + 20}) {
        const size_t i = (static_cast<size_t>(h / 2) * w + x) * 3 + 1;
        CHECK(v[i] == Approx(before[i]).epsilon(0.04));
    }
}

TEST_CASE("明瞭度: 新たに白飛びさせない", "[clarity]") {
    auto v = texture(200, 150, 0.6f, 0.6f);
    float max_before = 0;
    for (float x : v) max_before = std::max(max_before, x);
    REQUIRE(max_before <= 1.0f);
    REQUIRE(apply_local_contrast(v.data(), 200, 150, 1.0f, 20.0));
    for (float x : v) CHECK(x <= 1.0f + 1e-6f);
}

TEST_CASE("明瞭度: 一部の範囲を描いても全体を描いたときと同じ値になる（100% 表示）", "[clarity][pipeline]") {
    // 質感のあるプロキシ（センサー 1200 × 800、縮尺 1）
    const int sw = 1200, sh = 800;
    ImageF proxy(sw, sh);
    const auto tex = texture(sw, sh, 0.15f, 1.0f);
    std::copy(tex.begin(), tex.end(), proxy.data.begin());
    Settings s;
    s.clarity = 80;
    const ColorPipeline pipeline(s, srgb_camera());
    const GeometryPlan plan(sw, sh, 0, s.geometry);
    const SourceView src{nullptr, &proxy, sw, sh};

    std::vector<float> whole(static_cast<size_t>(sw) * sh * 3);
    REQUIRE(render_linear(src, plan, 1.0, {}, sw, sh, pipeline, Interpolation::Bilinear, whole.data()));
    const int rx = 413, ry = 290, rw = 300, rh = 200;
    std::vector<float> part(static_cast<size_t>(rw) * rh * 3);
    REQUIRE(render_linear(src, plan, 1.0, {static_cast<double>(rx), static_cast<double>(ry)}, rw, rh, pipeline,
                          Interpolation::Bilinear, part.data()));
    float max_diff = 0;
    for (int y = 0; y < rh; ++y)
        for (int x = 0; x < rw * 3; ++x)
            max_diff = std::max(max_diff, std::abs(part[static_cast<size_t>(y) * rw * 3 + x] -
                                                   whole[(static_cast<size_t>(y + ry) * sw + rx) * 3 + x]));
    CHECK(max_diff < 2e-3f);  // 8-bit で 1/2 段階未満

    // 明瞭度なしとは違う
    const ColorPipeline plain(Settings{}, srgb_camera());
    std::vector<float> none(static_cast<size_t>(rw) * rh * 3);
    render_linear(src, plan, 1.0, {static_cast<double>(rx), static_cast<double>(ry)}, rw, rh, plain,
                  Interpolation::Bilinear, none.data());
    CHECK(none != part);
}

TEST_CASE("明瞭度: ±200 は ±100 より強く効き、値は有限のまま", "[clarity]") {
    const int w = 300, h = 200;
    const auto base = texture(w, h);
    auto a100 = base, a200 = base, m200 = base;
    REQUIRE(apply_local_contrast(a100.data(), w, h, 1.0f, 20.0));
    REQUIRE(apply_local_contrast(a200.data(), w, h, 2.0f, 20.0));
    REQUIRE(apply_local_contrast(m200.data(), w, h, -2.0f, 20.0));
    CHECK(log_spread(a200, w, 50, 50, 250, 150) > log_spread(a100, w, 50, 50, 250, 150) * 1.1);
    CHECK(log_spread(m200, w, 50, 50, 250, 150) < log_spread(base, w, 50, 50, 250, 150) * 0.25);
    for (float v : a200) CHECK((std::isfinite(v) && v >= 0.0f && v <= 1.0f + 1e-6f));
    for (float v : m200) CHECK((std::isfinite(v) && v >= 0.0f));
}

TEST_CASE("明瞭度: 負は細かい明暗を弱めるだけで反転させない", "[clarity]") {
    const int w = 300, h = 200;
    const auto base = texture(w, h);
    const double s0 = log_spread(base, w, 50, 50, 250, 150);
    for (float amount : {-0.5f, -1.0f, -2.0f}) {
        auto v = base;
        REQUIRE(apply_local_contrast(v.data(), w, h, amount, 20.0));
        // 明暗の向きは元と同じ（中央の行で、元の明暗との相関が正）
        double corr = 0;
        for (int x = 50; x < 250; ++x) {
            const size_t i = (static_cast<size_t>(h / 2) * w + x) * 3 + 1;
            corr += (std::log2(base[i]) - std::log2(0.18)) * (std::log2(v[i]) - std::log2(0.18));
        }
        CHECK(corr > 0);
        const double s = log_spread(v, w, 50, 50, 250, 150);
        CHECK(s < s0);
        if (amount == -1.0f) CHECK(s == Approx(s0 * 0.5).margin(s0 * 0.15));
        if (amount == -2.0f) CHECK(s < s0 * 0.25);
    }
}
