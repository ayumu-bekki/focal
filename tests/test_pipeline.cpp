#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <vector>

#include "imaging/color_pipeline.h"
#include "imaging/output_transform.h"
#include "imaging/tone_curve.h"
#include "util/error.h"

using namespace focal;
using Catch::Approx;

namespace {

// rgb_cam = 単位行列（カメラ RGB = リニア sRGB）の色情報
ColorInfo srgb_camera() {
    ColorInfo c;
    c.rgb_cam = Mat3::identity();
    c.cam_xyz = rgb_to_xyz_matrix(primaries::kSrgb, primaries::kD65).inverse();
    return c;
}

} // namespace

TEST_CASE("ベースカーブ: 0→0、単調増加、肩で連続、16 付近で 1 に達する", "[tone]") {
    CHECK(base_curve(0.0) == 0.0);
    double prev = 0;
    for (double l = -20; l <= 4; l += 0.01) {
        const double y = base_curve(std::exp2(l));
        CHECK(y >= prev);
        prev = y;
    }
    CHECK(base_curve(16.0) > 0.999);
    CHECK(base_curve(16.0) <= 1.0);
    // 肩の開始点（0.4）で値が連続
    CHECK(base_curve(0.4 - 1e-9) == Approx(base_curve(0.4 + 1e-9)).margin(1e-6));
    // センサーの飽和（1.0）は十分明るく、中間グレーは中間調に
    CHECK(base_curve(1.0) > 0.85);
    CHECK(base_curve(0.18) == Approx(0.22).margin(0.03));
}

TEST_CASE("トーン: スライダーが既定値ならベースカーブそのもの", "[tone]") {
    const ToneParams p{};
    for (double x : {0.001, 0.01, 0.05, 0.18, 0.5, 1.0, 4.0})
        CHECK(tone_reference(x, p) == Approx(base_curve(x)).epsilon(1e-12));
}

TEST_CASE("トーン: 露出 +1EV は入力を 2 倍にするのと同じ", "[tone]") {
    ToneParams p{};
    p.exposure = 1.0;
    for (double x : {0.01, 0.1, 0.3}) CHECK(tone_reference(x, p) == Approx(base_curve(2 * x)).epsilon(1e-12));
}

TEST_CASE("トーン: コントラストは中間グレーを支点にする", "[tone]") {
    ToneParams p{};
    p.contrast = GENERATE(-100.0, -50.0, 50.0, 100.0);
    CHECK(tone_reference(0.18, p) == Approx(base_curve(0.18)).epsilon(1e-12));
    if (p.contrast > 0) {
        CHECK(tone_reference(0.5, p) > base_curve(0.5));
        CHECK(tone_reference(0.05, p) < base_curve(0.05));
    }
}

TEST_CASE("トーン: ハイライトは明部だけ、シャドウは暗部だけに効く", "[tone]") {
    ToneParams h{};
    h.highlights = -100;
    CHECK(tone_reference(0.05, h) == Approx(base_curve(0.05)).epsilon(1e-12));
    CHECK(tone_reference(1.0, h) < base_curve(1.0));

    ToneParams s{};
    s.shadows = 100;
    CHECK(tone_reference(0.5, s) == Approx(base_curve(0.5)).epsilon(1e-12));
    CHECK(tone_reference(0.02, s) > base_curve(0.02));
}

TEST_CASE("トーン LUT は厳密計算とほぼ一致する", "[tone]") {
    ToneParams p{};
    p.exposure = GENERATE(-2.0, 0.0, 1.5);
    p.contrast = 30;
    p.highlights = -50;
    p.shadows = 40;
    const ToneLut lut(p);
    double max_err = 0;
    for (double l = -14; l <= 5; l += 0.0137) {
        const double x = std::exp2(l);
        max_err = std::max(max_err, std::abs(lut.apply(static_cast<float>(x)) - tone_reference(x, p)));
    }
    CHECK(max_err < 2e-4);  // 8-bit 出力で 1/255 の 1/10 以下
    CHECK(lut.apply(0.0f) == 0.0f);
    CHECK(lut.apply(-1.0f) == 0.0f);
}

TEST_CASE("パイプライン: 既定値でグレーはグレーのまま、トーンだけがかかる", "[pipeline]") {
    const ColorPipeline p(Settings{}, srgb_camera());
    std::vector<float> rgb = {0.18f, 0.18f, 0.18f, 0.5f, 0.5f, 0.5f};
    p.process(rgb);
    CHECK(rgb[0] == Approx(base_curve(0.18)).margin(1e-4));
    CHECK(rgb[1] == Approx(rgb[0]).margin(1e-5));
    CHECK(rgb[2] == Approx(rgb[0]).margin(1e-5));
    CHECK(rgb[3] == Approx(base_curve(0.5)).margin(1e-4));
}

TEST_CASE("パイプライン: sRGB→Rec.2020 行列は白を保つ", "[pipeline]") {
    const Mat3 m = srgb_to_rec2020();
    for (int r = 0; r < 3; ++r) CHECK(m.m[r][0] + m.m[r][1] + m.m[r][2] == Approx(1.0).epsilon(1e-9));
    // ITU-R BT.2087 の値
    CHECK(m.m[0][0] == Approx(0.6274).margin(1e-4));
    CHECK(m.m[1][1] == Approx(0.9195).margin(1e-4));
    CHECK(m.m[2][2] == Approx(0.8956).margin(1e-4));
}

TEST_CASE("パイプライン: カスタム WB でも飽和画素はニュートラルのまま", "[pipeline]") {
    Settings s;
    s.wb.mode = WhiteBalanceSettings::Mode::Custom;
    s.wb.temperature = 3000;
    const ColorPipeline p(s, srgb_camera());
    std::vector<float> rgb = {1.0f, 1.0f, 1.0f};
    p.process(rgb);
    CHECK(rgb[0] == Approx(rgb[1]).margin(1e-5));
    CHECK(rgb[2] == Approx(rgb[1]).margin(1e-5));
}

TEST_CASE("パイプライン: 未対応の processVersion は拒否する", "[pipeline]") {
    Settings s;
    s.process_version = 2;
    CHECK_THROWS_AS(ColorPipeline(s, srgb_camera()), Error);
}

TEST_CASE("出力変換: sRGB は正式な区分関数で符号化される", "[output]") {
    const OutputTransform xf(OutputSpace::Srgb, OutputDepth::U8);
    // Rec.2020 のグレーは sRGB のグレー
    const float in[] = {0.18f, 0.18f, 0.18f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.002f, 0.002f, 0.002f};
    uint8_t out[12];
    xf.apply(in, out, 4);
    CHECK(int(out[0]) == 118);  // 0.18 → 0.4614 → 117.7
    CHECK(int(out[1]) == 118);
    CHECK(int(out[3]) == 255);
    CHECK(int(out[6]) == 0);
    CHECK(int(out[9]) == 7);  // 線形部 12.92 × 0.002 = 0.0258 → 6.6（γ2.2 なら 15）

    const auto icc = xf.icc_profile();
    REQUIRE(icc.size() > 128);
    CHECK(std::string(icc.begin() + 36, icc.begin() + 40) == "acsp");
}

TEST_CASE("出力変換: 16-bit と Display P3", "[output]") {
    const OutputTransform u16(OutputSpace::Srgb, OutputDepth::U16);
    const float white[] = {1.0f, 1.0f, 1.0f};
    uint16_t o16[3];
    u16.apply(white, o16, 1);
    CHECK(o16[0] == 65535);

    // P3 の純粋な緑（linear 0.5）を Rec.2020 で表して変換する。P3 では色域内、sRGB では色域外
    const Mat3 p3_to_2020 = rgb_to_xyz_matrix(primaries::kRec2020, primaries::kD65).inverse() *
                            rgb_to_xyz_matrix(primaries::kDisplayP3, primaries::kD65);
    const Vec3 g = p3_to_2020 * Vec3{{0.0, 0.5, 0.0}};
    const float green[] = {float(g[0]), float(g[1]), float(g[2])};
    const OutputTransform p3(OutputSpace::DisplayP3, OutputDepth::U8);
    const OutputTransform srgb(OutputSpace::Srgb, OutputDepth::U8);
    uint8_t a[3], b[3];
    p3.apply(green, a, 1);
    srgb.apply(green, b, 1);
    CHECK(int(a[0]) <= 1);
    CHECK(std::abs(int(a[1]) - 188) <= 1);  // 0.5 → 0.7354 → 187.5
    CHECK(int(a[2]) <= 1);
    CHECK(int(b[0]) == 0);  // sRGB では R が負になりクリップされる
    CHECK(int(b[1]) > int(a[1]));
}

TEST_CASE("出力変換: U8 の高速経路は lcms2 の変換と ±1 以内で一致する", "[output]") {
    for (OutputSpace space : {OutputSpace::Srgb, OutputSpace::DisplayP3}) {
        const OutputTransform xf(space, OutputDepth::U8);
        // 暗部を細かく含む疑似乱数の色（色域外・負値・1 超えも含む）
        std::vector<float> in;
        uint32_t seed = 12345;
        auto rnd = [&] {
            seed = seed * 1664525u + 1013904223u;
            return (seed >> 8) / 16777216.0f;
        };
        for (int i = 0; i < 20000; ++i) {
            const float scale = std::exp2(-12.0f * rnd());
            for (int c = 0; c < 3; ++c) in.push_back((rnd() * 1.3f - 0.15f) * scale);
        }
        const size_t n = in.size() / 3;
        std::vector<uint8_t> fast(in.size()), ref(in.size());
        xf.apply(in.data(), fast.data(), n);
        xf.apply_lcms(in.data(), ref.data(), n);
        int max_diff = 0;
        for (size_t i = 0; i < in.size(); ++i) max_diff = std::max(max_diff, std::abs(int(fast[i]) - int(ref[i])));
        CHECK(max_diff <= 1);
    }
}

TEST_CASE("出力変換: BGRX は RGB と同じ値を並べ替えて A=255 にしたもの", "[output]") {
    const OutputTransform xf(OutputSpace::DisplayP3, OutputDepth::U8);
    const float in[] = {0.1f, 0.5f, 0.9f, 1.2f, -0.1f, 0.3f};
    uint8_t rgb[6], bgrx[8];
    xf.apply(in, rgb, 2);
    xf.apply_bgrx(in, bgrx, 2);
    for (int i = 0; i < 2; ++i) {
        CHECK(bgrx[i * 4 + 0] == rgb[i * 3 + 2]);
        CHECK(bgrx[i * 4 + 1] == rgb[i * 3 + 1]);
        CHECK(bgrx[i * 4 + 2] == rgb[i * 3 + 0]);
        CHECK(bgrx[i * 4 + 3] == 255);
    }
}

TEST_CASE("トーン: 白は明部の端、黒は暗部の端だけに効き、中間グレーは動かさない", "[tone]") {
    ToneParams w{};
    w.whites = GENERATE(-100.0, 100.0);
    CHECK(tone_reference(0.18, w) == Approx(base_curve(0.18)).epsilon(1e-12));
    CHECK(tone_reference(0.02, w) == Approx(base_curve(0.02)).epsilon(1e-12));
    if (w.whites > 0) CHECK(tone_reference(2.0, w) > base_curve(2.0));
    if (w.whites < 0) CHECK(tone_reference(2.0, w) < base_curve(2.0));

    ToneParams b{};
    b.blacks = GENERATE(-100.0, 100.0);
    CHECK(tone_reference(0.18, b) == Approx(base_curve(0.18)).epsilon(1e-12));
    CHECK(tone_reference(1.0, b) == Approx(base_curve(1.0)).epsilon(1e-12));
    if (b.blacks > 0) CHECK(tone_reference(0.003, b) > base_curve(0.003));
    if (b.blacks < 0) CHECK(tone_reference(0.003, b) < base_curve(0.003));
}

TEST_CASE("トーン: 明るさは 0 と 1 を動かさず中間調を上下させ、単調増加のまま", "[tone]") {
    ToneParams p{};
    p.brightness = GENERATE(-100.0, -30.0, 40.0, 100.0);
    CHECK(tone_reference(0.0, p) == 0.0);
    CHECK(tone_reference(1000.0, p) == Approx(tone_reference(1000.0, ToneParams{})).epsilon(1e-9));
    if (p.brightness > 0) CHECK(tone_reference(0.18, p) > base_curve(0.18));
    if (p.brightness < 0) CHECK(tone_reference(0.18, p) < base_curve(0.18));
    double prev = -1;
    for (double l = -14; l <= 6; l += 0.05) {
        const double y = tone_reference(std::exp2(l), p);
        CHECK(y >= prev);
        prev = y;
    }
}

TEST_CASE("パイプライン: 彩度 -100 はモノクロ、グレーは彩度・自然な彩度で変わらない", "[pipeline]") {
    Settings s;
    s.saturation = -100;
    const ColorPipeline mono(s, srgb_camera());
    std::vector<float> rgb = {0.4f, 0.1f, 0.05f};
    mono.process(rgb);
    CHECK(rgb[0] == Approx(rgb[1]).margin(1e-5));
    CHECK(rgb[1] == Approx(rgb[2]).margin(1e-5));

    s.saturation = 60;
    s.vibrance = 80;
    const ColorPipeline vivid(s, srgb_camera());
    std::vector<float> gray = {0.18f, 0.18f, 0.18f};
    vivid.process(gray);
    CHECK(gray[0] == Approx(base_curve(0.18)).margin(1e-4));
    CHECK(gray[1] == Approx(gray[0]).margin(1e-5));
    CHECK(gray[2] == Approx(gray[0]).margin(1e-5));
}

namespace {
// 処理後の鮮やかさ（max - min）/ max
float chroma_after(const ColorPipeline& p, std::vector<float> rgb) {
    p.process(rgb);
    const float mx = std::max({rgb[0], rgb[1], rgb[2]}), mn = std::min({rgb[0], rgb[1], rgb[2]});
    return (mx - mn) / mx;
}
} // namespace

TEST_CASE("パイプライン: 自然な彩度は鮮やかさの低い色ほど強く、肌色は控えめ", "[pipeline]") {
    Settings s;
    const ColorPipeline base(s, srgb_camera());
    s.vibrance = 100;
    const ColorPipeline vib(s, srgb_camera());

    // くすんだ青と鮮やかな青: 増え方はくすんだ方が大きい
    const std::vector<float> dull = {0.15f, 0.17f, 0.22f}, vivid = {0.02f, 0.05f, 0.4f};
    const float dull_gain = chroma_after(vib, dull) / chroma_after(base, dull);
    const float vivid_gain = chroma_after(vib, vivid) / chroma_after(base, vivid);
    CHECK(dull_gain > 1.05f);
    CHECK(dull_gain > vivid_gain);

    // 同じくらいくすんだ肌色（橙）と青: 肌色の方が増え方が小さい
    const std::vector<float> skin = {0.22f, 0.17f, 0.14f};
    const float skin_gain = chroma_after(vib, skin) / chroma_after(base, skin);
    CHECK(skin_gain > 1.0f);
    CHECK(skin_gain < dull_gain);
}
