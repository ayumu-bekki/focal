#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <numbers>

#include "imaging/geometry.h"

using namespace focal;
using Catch::Approx;

namespace {

// LibRaw の flip_index と同じ対応（write_ph.cpp）
void libraw_flip_index(int flip, int iwidth, int iheight, int row, int col, int& srow, int& scol) {
    if (flip & 4) std::swap(row, col);
    if (flip & 2) row = iheight - 1 - row;
    if (flip & 1) col = iwidth - 1 - col;
    srow = row;
    scol = col;
}

void check_same_mapping(const GeometryPlan& a, const GeometryPlan& b, int w, int h) {
    const Affine ma = a.output_to_sensor(1.0), mb = b.output_to_sensor(1.0);
    for (int y = 0; y < h; y += 3)
        for (int x = 0; x < w; x += 3) {
            const PointD pa = ma.apply({x + 0.5, y + 0.5}), pb = mb.apply({x + 0.5, y + 0.5});
            REQUIRE(pa.x == Approx(pb.x).margin(1e-9));
            REQUIRE(pa.y == Approx(pb.y).margin(1e-9));
        }
}

} // namespace

TEST_CASE("Affine の合成と逆変換", "[geometry]") {
    const Affine a{2, 1, 3, -1, 0.5, 7};
    const Affine id = a.inverse().after(a);
    CHECK(id.a == Approx(1));
    CHECK(id.b == Approx(0).margin(1e-12));
    CHECK(id.c == Approx(0).margin(1e-12));
    CHECK(id.e == Approx(1));
    CHECK(id.f == Approx(0).margin(1e-12));
}

TEST_CASE("(a) 向き補正は LibRaw の flip_index と一致する", "[geometry]") {
    const int flip = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    const int sw = 13, sh = 7;
    const GeometryPlan plan(sw, sh, flip, GeometrySettings{});
    const int ow = (flip & 4) ? sh : sw, oh = (flip & 4) ? sw : sh;
    CHECK(plan.output_width() == ow);
    CHECK(plan.output_height() == oh);
    const Affine m = plan.output_to_sensor(1.0);
    for (int row = 0; row < oh; ++row)
        for (int col = 0; col < ow; ++col) {
            int sr, sc;
            libraw_flip_index(flip, sw, sh, row, col, sr, sc);
            const PointD p = m.apply({col + 0.5, row + 0.5});
            REQUIRE(p.x == Approx(sc + 0.5));
            REQUIRE(p.y == Approx(sr + 0.5));
        }
}

TEST_CASE("(b) 90° 回転は flip の回転と同じ対応になる", "[geometry]") {
    // flip 6 = 時計回り 90°、3 = 180°、5 = 反時計回り 90°
    const int sw = 11, sh = 5;
    GeometrySettings g;
    g.rotate90 = 1;
    check_same_mapping(GeometryPlan(sw, sh, 0, g), GeometryPlan(sw, sh, 6, {}), sh, sw);
    g.rotate90 = 2;
    check_same_mapping(GeometryPlan(sw, sh, 0, g), GeometryPlan(sw, sh, 3, {}), sw, sh);
    g.rotate90 = 3;
    check_same_mapping(GeometryPlan(sw, sh, 0, g), GeometryPlan(sw, sh, 5, {}), sh, sw);
    // flip 6 + 反時計回り 90° は元に戻る
    check_same_mapping(GeometryPlan(sw, sh, 6, g), GeometryPlan(sw, sh, 0, {}), sw, sh);
}

TEST_CASE("(c) 傾き補正: 正の角度で画像が時計回りに回る", "[geometry]") {
    const int sw = 400, sh = 300;
    GeometrySettings g;
    g.straighten = 10;
    const GeometryPlan plan(sw, sh, 0, g);
    const Affine m = plan.output_to_sensor(1.0);
    const double th = 10 * std::numbers::pi / 180, d = 100;
    // ソースの中心の右 d の点は、出力では時計回りに θ 回った位置（y 下向きなので下に動く）
    const PointD c = m.apply({200, 150});
    CHECK(c.x == Approx(200));
    CHECK(c.y == Approx(150));
    const PointD p = m.apply({200 + d * std::cos(th), 150 + d * std::sin(th)});
    CHECK(p.x == Approx(300));
    CHECK(p.y == Approx(150).margin(1e-9));
}

TEST_CASE("(d) クロップと縮尺・部分描画の原点", "[geometry]") {
    GeometrySettings g;
    g.crop = {0.25, 0.5, 0.5, 0.25};
    const GeometryPlan plan(800, 400, 0, g);
    CHECK(plan.output_width() == 400);
    CHECK(plan.output_height() == 100);
    // 等倍
    PointD p = plan.output_to_sensor(1.0).apply({0.5, 0.5});
    CHECK(p.x == Approx(200.5));
    CHECK(p.y == Approx(200.5));
    // 1/2 縮小: 出力 1 画素 = センサー 2 画素
    p = plan.output_to_sensor(0.5).apply({1.0, 1.0});
    CHECK(p.x == Approx(202));
    CHECK(p.y == Approx(202));
    // 100% 表示で (30, 20) から始まる領域
    p = plan.output_to_sensor(1.0, {30, 20}).apply({0.5, 0.5});
    CHECK(p.x == Approx(230.5));
    CHECK(p.y == Approx(220.5));

    // クロップモード中は (d) を適用しない
    const GeometryPlan whole(800, 400, 0, g, false);
    CHECK(whole.output_width() == 800);
    CHECK(whole.output_height() == 400);
}

TEST_CASE("回転＋クロップの合成", "[geometry]") {
    // 時計回り 90° 回転後の左上 1/4 は、センサーの左下 1/4
    GeometrySettings g;
    g.rotate90 = 1;
    g.crop = {0, 0, 0.5, 0.5};
    const GeometryPlan plan(600, 400, 0, g);
    CHECK(plan.output_width() == 200);
    CHECK(plan.output_height() == 300);
    const PointD p = plan.output_to_sensor(1.0).apply({0.5, 0.5});
    CHECK(p.x == Approx(0.5));
    CHECK(p.y == Approx(399.5));
}

TEST_CASE("自動クロップ: 四隅が画像内に収まる最大の枠", "[geometry]") {
    const double cw = 6000, ch = 4000;
    const double deg = GENERATE(-45.0, -12.5, -1.0, 0.5, 3.0, 30.0, 45.0);
    const CropRect full{};
    const CropRect r = fit_crop_to_straighten(full, deg, cw, ch);

    // 縦横比と中心を保つ
    CHECK(r.w / r.h == Approx(1.0));
    CHECK(r.x + r.w / 2 == Approx(0.5));
    CHECK(r.y + r.h / 2 == Approx(0.5));

    // 解析解: 同じ縦横比の枠の縮小率 s = 1 / (cosθ + sinθ × 長辺/短辺)
    const double th = std::abs(deg) * std::numbers::pi / 180;
    const double expected = 1.0 / (std::cos(th) + std::sin(th) * cw / ch);
    CHECK(r.w == Approx(expected).epsilon(1e-6));

    GeometrySettings g;
    g.straighten = deg;
    g.crop = r;
    const GeometryPlan plan(static_cast<int>(cw), static_cast<int>(ch), 0, g);
    const Affine m = plan.output_to_sensor(1.0);
    const double ow = plan.output_width(), oh = plan.output_height();
    for (PointD corner : {PointD{0, 0}, PointD{ow, 0}, PointD{0, oh}, PointD{ow, oh}}) {
        const PointD p = m.apply(corner);
        CHECK(p.x >= -1e-6);
        CHECK(p.y >= -1e-6);
        CHECK(p.x <= cw + 1e-6);
        CHECK(p.y <= ch + 1e-6);
    }
}

TEST_CASE("自動クロップ: 傾きがなければそのまま、小さい枠は縮めない", "[geometry]") {
    const CropRect c{0.1, 0.2, 0.3, 0.4};
    CHECK(fit_crop_to_straighten(c, 0.0, 600, 400) == c);
    const CropRect small{0.45, 0.45, 0.1, 0.1};
    CHECK(fit_crop_to_straighten(small, 5.0, 600, 400) == small);
}
