#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>

#include "imaging/crop_tool.h"

using namespace focal;
using Catch::Approx;

namespace {
constexpr double W = 6000, H = 4000;
double px_aspect(const CropRect& c) { return c.w * W / (c.h * H); }
bool near(const CropRect& a, const CropRect& b) {
    return std::abs(a.x - b.x) < 1e-9 && std::abs(a.y - b.y) < 1e-9 && std::abs(a.w - b.w) < 1e-9 &&
           std::abs(a.h - b.h) < 1e-9;
}
} // namespace

TEST_CASE("max_crop: 傾きなしなら縦横比どおりの最大の枠", "[crop]") {
    const CropRect full = max_crop(0, 0, W, H);
    CHECK(full == CropRect{0, 0, 1, 1});
    const CropRect sq = max_crop(1.0, 0, W, H);
    CHECK(sq.w * W == Approx(4000));
    CHECK(sq.h == Approx(1));
    CHECK(sq.x == Approx(1.0 / 6));
}

TEST_CASE("max_crop: 傾き補正しても四隅が画像内に収まり、縦横比を保つ", "[crop]") {
    const double deg = GENERATE(-30.0, -5.0, 3.0, 12.0, 45.0);
    const double aspect = GENERATE(0.0, 1.0, 1.5, 16.0 / 9);
    const CropRect c = max_crop(aspect, deg, W, H);
    CHECK(crop_inside_image(c, deg, W, H));
    CHECK(px_aspect(c) == Approx(aspect == 0 ? W / H : aspect).epsilon(1e-6));
    // 最大であること: 少し大きくすると収まらない
    CropRect bigger = c;
    bigger.w *= 1.01;
    bigger.h *= 1.01;
    bigger.x -= c.w * 0.005;
    bigger.y -= c.h * 0.005;
    CHECK_FALSE(crop_inside_image(bigger, deg, W, H));
}

TEST_CASE("drag_crop: 角のドラッグで反対の角は動かず、縦横比を保つ", "[crop]") {
    const CropRect start{0.2, 0.2, 0.6, 0.6};
    const CropRect r = drag_crop(start, CropHandle::BottomRight, -0.2, -0.05, 1.5, 0, W, H);
    CHECK(r.x == Approx(0.2));
    CHECK(r.y == Approx(0.2));
    CHECK(px_aspect(r) == Approx(1.5));
    // 自由比率なら両方そのまま動く
    const CropRect f = drag_crop(start, CropHandle::TopLeft, 0.1, 0.05, 0, 0, W, H);
    CHECK(near(f, CropRect{0.3, 0.25, 0.5, 0.55}));
}

TEST_CASE("drag_crop: 辺のドラッグは反対の辺を固定、縦横比固定なら中央を保つ", "[crop]") {
    const CropRect start{0.2, 0.2, 0.6, 0.6};
    const CropRect r = drag_crop(start, CropHandle::Left, 0.1, 0.4, 0, 0, W, H);
    CHECK(r.x == Approx(0.3));
    CHECK(r.x + r.w == Approx(0.8));
    CHECK(r.y == Approx(0.2));  // 縦方向の移動は無視
    const CropRect a = drag_crop(start, CropHandle::Right, -0.1, 0, 1.0, 0, W, H);
    CHECK(px_aspect(a) == Approx(1.0));
    CHECK(a.y + a.h / 2 == Approx(0.5));
}

TEST_CASE("drag_crop: 画像の外や反対側を越える操作は収まる範囲で止まる", "[crop]") {
    const CropRect start{0.2, 0.2, 0.6, 0.6};
    // 移動: 端を越えない
    const CropRect m = drag_crop(start, CropHandle::Move, 0.5, -0.5, 0, 0, W, H);
    CHECK(m.x + m.w == Approx(1.0));
    CHECK(m.y == Approx(0.0));
    CHECK(m.w == Approx(0.6));
    // 反対側を越えても裏返らない（最小の大きさで止まる）
    const CropRect t = drag_crop(start, CropHandle::Left, 0.9, 0, 0, 0, W, H);
    CHECK(t.w > 0);
    CHECK(t.x + t.w == Approx(0.8));
    // 傾き補正中は回転後の画像の外に出ない
    const CropRect inside = max_crop(0, 10, W, H);
    const CropRect g = drag_crop(inside, CropHandle::TopLeft, -0.3, -0.3, 0, 10, W, H);
    CHECK(crop_inside_image(g, 10, W, H));
}

TEST_CASE("rotate_crop: 4 回で元に戻り、時計回りに回る", "[crop]") {
    const CropRect c{0.1, 0.2, 0.3, 0.4};
    CHECK(near(rotate_crop(c, 4), c));
    CHECK(rotate_crop(c, 0) == c);
    const CropRect r = rotate_crop(c, 1);
    // 左上寄りの枠は、時計回りに回すと右上寄りになる
    CHECK(r.x == Approx(1 - 0.6));
    CHECK(r.y == Approx(0.1));
    CHECK(r.w == Approx(0.4));
    CHECK(r.h == Approx(0.3));
    CHECK(near(rotate_crop(rotate_crop(c, 1), -1), c));
}

TEST_CASE("straighten_from_line: 線の傾きを打ち消す", "[crop]") {
    // 右下がり 10° の線 → 反時計回りに 10°（-10）
    const double t = std::tan(10 * 3.14159265358979 / 180);
    CHECK(straighten_from_line({0, 0}, {100, 100 * t}, 0) == Approx(-10).margin(1e-6));
    // 逆向きに引いても同じ
    CHECK(straighten_from_line({100, 100 * t}, {0, 0}, 0) == Approx(-10).margin(1e-6));
    // 縦に近い線は垂直を基準にする（右に 5° 傾いた柱）
    const double v = std::tan(5 * 3.14159265358979 / 180);
    CHECK(straighten_from_line({0, 0}, {-100 * v, 100}, 2) == Approx(2 - 5).margin(1e-6));
    // ±45 に収める
    CHECK(straighten_from_line({0, 0}, {100, -80}, -40) == Approx(-1.34).margin(0.05));
    CHECK(straighten_from_line({0, 0}, {0, 0}, 7) == 7);
}
