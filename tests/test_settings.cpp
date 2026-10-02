#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "edit/settings.h"
#include "util/error.h"

using namespace focal;
using nlohmann::json;

TEST_CASE("空の JSON はすべて既定値になる", "[settings]") {
    const Settings s = settings_from_json("{}");
    CHECK(s.is_default());
    CHECK(s.process_version == 1);
    CHECK(s.wb.mode == WhiteBalanceSettings::Mode::AsShot);
    CHECK(s.geometry.crop == CropRect{});
}

TEST_CASE("既知のキーを読み込み、往復で値が変わらない", "[settings]") {
    const char* text = R"({
        "schema": 1, "processVersion": 1,
        "wb": {"mode": "custom", "temperature": 4800, "tint": 12},
        "exposure": 1.5, "contrast": 20, "highlights": -30, "shadows": 40,
        "geometry": {"rotate90": 3, "straighten": -2.5,
                     "crop": {"x": 0.1, "y": 0.2, "w": 0.5, "h": 0.6}, "aspect": "3:2"}
    })";
    const Settings s = settings_from_json(text);
    CHECK(s.wb.mode == WhiteBalanceSettings::Mode::Custom);
    CHECK(s.wb.temperature == 4800);
    CHECK(s.wb.tint == 12);
    CHECK(s.exposure == 1.5);
    CHECK(s.contrast == 20);
    CHECK(s.highlights == -30);
    CHECK(s.shadows == 40);
    CHECK(s.geometry.rotate90 == 3);
    CHECK(s.geometry.straighten == -2.5);
    CHECK(s.geometry.crop == CropRect{0.1, 0.2, 0.5, 0.6});
    CHECK(s.geometry.aspect == AspectMode::R3x2);

    const Settings again = settings_from_json(settings_to_json(s));
    CHECK(again == s);
}

TEST_CASE("未知のキーは保存の往復で保持される", "[settings]") {
    const char* text = R"({
        "exposure": 1.0,
        "futureSlider": 42,
        "wb": {"mode": "asShot", "futureWbKey": "x"},
        "geometry": {"crop": {"x": 0, "y": 0, "w": 1, "h": 1, "futureCropKey": true}, "futureGeoKey": [1, 2]}
    })";
    Settings s = settings_from_json(text);
    s.exposure = -0.5;
    const json out = json::parse(settings_to_json(s));
    CHECK(out["futureSlider"] == 42);
    CHECK(out["wb"]["futureWbKey"] == "x");
    CHECK(out["geometry"]["futureGeoKey"] == json::array({1, 2}));
    CHECK(out["geometry"]["crop"]["futureCropKey"] == true);
    CHECK(out["exposure"] == -0.5);
    CHECK(out["schema"] == 1);
}

TEST_CASE("asShot に戻すと temperature / tint は書き出さない", "[settings]") {
    Settings s = settings_from_json(R"({"wb": {"mode": "custom", "temperature": 3000, "tint": 5}})");
    s.wb.mode = WhiteBalanceSettings::Mode::AsShot;
    const json out = json::parse(settings_to_json(s));
    CHECK(out["wb"]["mode"] == "asShot");
    CHECK_FALSE(out["wb"].contains("temperature"));
    CHECK_FALSE(out["wb"].contains("tint"));
}

TEST_CASE("範囲外・型違いの値は既定値か範囲内に収める", "[settings]") {
    const Settings s = settings_from_json(R"({
        "exposure": 99, "contrast": "high", "shadows": -1000,
        "wb": {"mode": "custom", "temperature": 100},
        "geometry": {"rotate90": 5, "straighten": 90, "crop": {"x": 0.8, "y": -1, "w": 0.5, "h": 2},
                     "aspect": "7:3"}
    })");
    CHECK(s.exposure == 5.0);
    CHECK(s.contrast == 0.0);
    CHECK(s.shadows == -100.0);
    CHECK(s.wb.temperature == 2000.0);
    CHECK(s.geometry.rotate90 == 1);
    CHECK(s.geometry.straighten == 45.0);
    CHECK(s.geometry.crop.x + s.geometry.crop.w <= 1.0);
    CHECK(s.geometry.crop.y == 0.0);
    CHECK(s.geometry.crop.h == 1.0);
    CHECK(s.geometry.aspect == AspectMode::Free);
}

TEST_CASE("壊れた JSON は例外になる", "[settings]") {
    CHECK_THROWS_AS(settings_from_json("{not json"), Error);
    CHECK_THROWS_AS(settings_from_json("[1,2]"), Error);
}

TEST_CASE("白・黒・明るさ・彩度・自然な彩度: 往復し、範囲に収め、既定値なら書かない", "[settings]") {
    Settings s = settings_from_json(
        R"({"whites": 30, "blacks": -20, "brightness": 15, "saturation": -100, "vibrance": 250})");
    CHECK(s.whites == 30);
    CHECK(s.blacks == -20);
    CHECK(s.brightness == 15);
    CHECK(s.saturation == -100);
    CHECK(s.vibrance == 100);  // 範囲内に収める
    CHECK_FALSE(s.is_default());
    CHECK(settings_from_json(settings_to_json(s)) == s);

    // 既定値に戻すとキーを書かず、行も要らない（古い版の JSON と同じ形）
    s.whites = s.blacks = s.brightness = s.saturation = s.vibrance = 0;
    const json doc = json::parse(settings_to_json(s));
    for (const char* k : {"whites", "blacks", "brightness", "saturation", "vibrance"}) CHECK_FALSE(doc.contains(k));
    CHECK(settings_need_no_row(s));
}

TEST_CASE("明瞭度: 往復し、範囲に収め、既定値なら書かない", "[settings]") {
    Settings s = settings_from_json(R"({"clarity": 250})");
    CHECK(s.clarity == 200);  // 明瞭度だけ ±200（v3.12）
    CHECK(settings_from_json(R"({"clarity": -150})").clarity == -150);
    CHECK(settings_from_json(settings_to_json(s)) == s);
    s.clarity = 0;
    CHECK_FALSE(json::parse(settings_to_json(s)).contains("clarity"));
    CHECK(settings_need_no_row(s));
}

TEST_CASE("シャープネス・ノイズ低減: 往復し、範囲に収め、既定値なら書かない", "[settings]") {
    Settings s = settings_from_json(R"({"sharpness": 200, "noiseReduction": 40, "colorNoiseReduction": -5})");
    CHECK(s.sharpness == 150);
    CHECK(s.noise_reduction == 40);
    CHECK(s.color_noise_reduction == 0);
    CHECK(settings_from_json(settings_to_json(s)) == s);
    s.sharpness = s.noise_reduction = 0;
    const json doc = json::parse(settings_to_json(s));
    for (const char* k : {"sharpness", "noiseReduction", "colorNoiseReduction"}) CHECK_FALSE(doc.contains(k));
    CHECK(settings_need_no_row(s));
}
