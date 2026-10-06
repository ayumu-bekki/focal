#pragma once

#include <string>
#include <string_view>

namespace focal {

// 編集パラメータ（6.1 章、schema 1）。

struct WhiteBalanceSettings {
    enum class Mode { AsShot, Custom };
    Mode mode = Mode::AsShot;
    double temperature = 5500.0;  // K（custom 時のみ有効）
    double tint = 0.0;            // custom 時のみ有効

    bool operator==(const WhiteBalanceSettings&) const = default;
};

struct CropRect {
    double x = 0.0, y = 0.0, w = 1.0, h = 1.0;  // (c) 後の画像に対する正規化矩形
    bool operator==(const CropRect&) const = default;
};

enum class AspectMode { Free, Original, R1x1, R3x2, R4x3, R16x9, R5x4 };

struct GeometrySettings {
    int rotate90 = 0;         // 0..3、時計回り
    double straighten = 0.0;  // -45..+45 度。正の値で画像を時計回りに回す
    CropRect crop;
    AspectMode aspect = AspectMode::Free;

    bool operator==(const GeometrySettings&) const = default;
};

// レンズ補正の出力の射影（v3.27、Lensfun）。Keep はレンズ本来の射影のまま
enum class LensProjection { Keep, Rectilinear, Fisheye, Equisolid, Stereographic, Orthographic, Panoramic, Equirectangular };

// レンズ補正（v3.27、design.md 5.13 章）。enabled が false なら何もしない。
// id が空なら写真のレンズ名から自動で選ぶ。それ以外は "メーカー|モデル"（Lensfun の DB のレンズ）
struct LensSettings {
    bool enabled = false;
    std::string id;
    double distortion = 100.0;   // 0..100 歪曲収差（射影の変換・自動拡大も含む）
    double tca = 100.0;          // 0..100 倍率色収差
    double vignetting = 100.0;   // 0..100 周辺減光
    LensProjection projection = LensProjection::Keep;

    bool operator==(const LensSettings&) const = default;
};

struct Settings {
    static constexpr int kSchema = 1;
    static constexpr int kLatestProcessVersion = 1;

    int process_version = kLatestProcessVersion;
    WhiteBalanceSettings wb;
    double exposure = 0.0;    // EV
    double contrast = 0.0;    // -100..100
    double highlights = 0.0;  // -100..100
    double shadows = 0.0;     // -100..100
    // v3.9（process_version 1 のまま。既定値 0 では何もしない）
    double whites = 0.0;      // -100..100 最も明るい部分（白の位置）
    double blacks = 0.0;      // -100..100 最も暗い部分（黒の位置）
    double brightness = 0.0;  // -100..100 中間調の明るさ（白と黒の位置は変えない）
    double saturation = 0.0;  // -100..100 彩度（-100 でモノクロ）
    double vibrance = 0.0;    // -100..100 自然な彩度（鮮やかさの低い色ほど強く、肌色は控えめ）
    double clarity = 0.0;     // -200..200 明瞭度（局所コントラスト。負で柔らかく。v3.12 で ±100 から広げた）
    // v3.13（既定値 0 では何もしない）。効き方は 100% 表示と書き出しで見える
    double sharpness = 0.0;              // 0..150 シャープネス
    double noise_reduction = 0.0;        // 0..100 ノイズ低減（輝度）
    double color_noise_reduction = 0.0;  // 0..100 ノイズ低減（カラー）
    LensSettings lens;
    GeometrySettings geometry;

    // 読み込んだ JSON の原文。未知のキーを保存時に保持するために使う（6.1 章）。
    std::string preserved_json;

    // 既知のパラメータだけで比較する
    bool operator==(const Settings& o) const {
        return process_version == o.process_version && wb == o.wb && exposure == o.exposure &&
               contrast == o.contrast && highlights == o.highlights && shadows == o.shadows &&
               whites == o.whites && blacks == o.blacks && brightness == o.brightness &&
               saturation == o.saturation && vibrance == o.vibrance && clarity == o.clarity &&
               sharpness == o.sharpness && noise_reduction == o.noise_reduction &&
               color_noise_reduction == o.color_noise_reduction &&
               lens == o.lens && geometry == o.geometry;
    }

    bool is_default() const { return *this == Settings{}; }

    // 範囲外の値を範囲内に収める
    void clamp();
};

// 存在しないキーは既定値、型が違う値も既定値、範囲外は clamp する。
// JSON として壊れている場合は Error を投げる。
Settings settings_from_json(std::string_view json);
std::string settings_to_json(const Settings& settings);

// 既知のパラメータがすべて既定値で、読み込んだ JSON に未知のキーも残っていない（= DB に行が要らない、6.1 章）
bool settings_need_no_row(const Settings& settings);

const char* aspect_to_string(AspectMode a);
bool aspect_from_string(std::string_view s, AspectMode& out);
const char* lens_projection_to_string(LensProjection p);
bool lens_projection_from_string(std::string_view s, LensProjection& out);

} // namespace focal
