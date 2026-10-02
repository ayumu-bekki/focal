#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "imaging/color_pipeline.h"
#include "imaging/white_balance.h"

using namespace focal;
using Catch::Approx;

namespace {

// 典型的なカメラ（Sony ILCE-7M3 の adobe_coeff 相当）の色情報を作る
ColorInfo sample_color_info() {
    ColorInfo c;
    const double cam_xyz[3][3] = {{0.7374, -0.2389, -0.0551}, {-0.5435, 1.3162, 0.2519}, {-0.1006, 0.1795, 0.6552}};
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) c.cam_xyz.m[r][k] = cam_xyz[r][k];
    // dcraw の cam_xyz_coeff と同じ手順で rgb_cam と昼光係数を求める
    const Mat3 xyz_rgb = rgb_to_xyz_matrix(primaries::kSrgb, primaries::kD65);
    Mat3 cam_rgb = c.cam_xyz * xyz_rgb;
    double pre[3];
    for (int r = 0; r < 3; ++r) {
        const double sum = cam_rgb.m[r][0] + cam_rgb.m[r][1] + cam_rgb.m[r][2];
        for (int k = 0; k < 3; ++k) cam_rgb.m[r][k] /= sum;
        pre[r] = 1.0 / sum;
    }
    c.rgb_cam = cam_rgb.inverse();
    c.daylight_wb = {pre[0] / pre[1], 1.0, pre[2] / pre[1]};
    c.as_shot_wb = {2.2, 1.0, 1.6};
    return c;
}

} // namespace

TEST_CASE("Robertson: 黒体軌跡上の既知の点", "[wb]") {
    // 6504K 付近は D65 に近い（D65 は軌跡からわずかに緑寄り）
    const ChromaXy p = temp_tint_to_xy(6504, 0);
    CHECK(p.x == Approx(0.3135).margin(0.002));
    CHECK(p.y == Approx(0.3237).margin(0.002));
    // 2856K（A 光源）は黒体軌跡上
    const ChromaXy a = temp_tint_to_xy(2856, 0);
    CHECK(a.x == Approx(0.4476).margin(0.002));
    CHECK(a.y == Approx(0.4074).margin(0.002));
}

TEST_CASE("K/tint → xy → K/tint の往復", "[wb]") {
    const double k = GENERATE(2000.0, 2856.0, 4000.0, 5500.0, 6504.0, 10000.0, 25000.0, 50000.0);
    const double tint = GENERATE(-150.0, -40.0, 0.0, 25.0, 150.0);
    double k2 = 0, t2 = 0;
    xy_to_temp_tint(temp_tint_to_xy(k, tint), k2, t2);
    // 表の区間内で順方向と逆方向の補間が厳密には対称でないため、わずかな誤差が残る
    CHECK(k2 == Approx(k).epsilon(1e-5));
    CHECK(t2 == Approx(tint).margin(1e-4));
}

TEST_CASE("K/tint → WB 係数 → K/tint の往復", "[wb]") {
    const ColorInfo c = sample_color_info();
    const double k = GENERATE(2000.0, 3200.0, 5000.0, 6500.0, 9000.0, 20000.0, 50000.0);
    const double tint = GENERATE(-100.0, 0.0, 60.0);
    const auto wb = wb_from_temp_tint(k, tint, c);
    CHECK(wb[1] == 1.0);
    double k2 = 0, t2 = 0;
    temp_tint_from_wb(wb, c, k2, t2);
    // 相対誤差 0.1% 以内（2000K で ±2K、50000K で ±50K）
    CHECK(k2 == Approx(k).epsilon(1e-3));
    CHECK(t2 == Approx(tint).margin(0.1));
}

TEST_CASE("D65 の WB 係数はカメラの昼光係数と一致する", "[wb]") {
    const ColorInfo c = sample_color_info();
    double k = 0, tint = 0;
    temp_tint_from_wb(c.daylight_wb, c, k, tint);
    const auto wb = wb_from_temp_tint(k, tint, c);
    CHECK(wb[0] == Approx(c.daylight_wb[0]).epsilon(1e-6));
    CHECK(wb[2] == Approx(c.daylight_wb[2]).epsilon(1e-6));
    CHECK(k == Approx(6504).margin(80));
}

TEST_CASE("色温度を上げると R の係数が増え B の係数が減る（暖色化）", "[wb]") {
    const ColorInfo c = sample_color_info();
    const auto cool = wb_from_temp_tint(3000, 0, c);
    const auto warm = wb_from_temp_tint(8000, 0, c);
    CHECK(warm[0] > cool[0]);
    CHECK(warm[2] < cool[2]);
}

TEST_CASE("As Shot では WB 比が 1", "[wb][pipeline]") {
    const ColorInfo c = sample_color_info();
    const ColorPipeline p(Settings{}, c);
    for (float r : p.wb_ratio()) CHECK(r == 1.0f);
}
