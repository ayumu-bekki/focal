#pragma once

#include <array>
#include <span>

#include "edit/settings.h"
#include "imaging/detail.h"
#include "imaging/raw_decoder.h"
#include "imaging/tone_curve.h"
#include "util/matrix.h"

namespace focal {

// 5.4 章の (2)〜(5b)。プレビュー・100% 表示・書き出し・サムネ再生成のすべてで同じものを使う（ADR-03）。
// 設定から導出される値（WB 比、合成行列、トーン LUT）は構築時に 1 回だけ計算する。
class ColorPipeline {
public:
    ColorPipeline(const Settings& settings, const ColorInfo& color);

    // rgb: インターリーブ RGB。入力はカメラ RGB（As Shot WB 適用済み、0..1）、
    // 出力はトーン適用後のリニア Rec.2020（0..1）。in-place で処理する。
    void process(std::span<float> rgb) const;

    // 明瞭度があるときは、process を 2 つに分けて間に局所コントラスト（5a）を挟む（renderer が行う）
    void process_tone(std::span<float> rgb) const;   // (2)〜(5)
    void process_color(std::span<float> rgb) const;  // (5b)
    float clarity() const { return clarity_; }       // -2..2
    const DetailParams& detail() const { return detail_; }  // ノイズ低減・シャープネス
    // 周辺画素を使う処理（5a）があるか
    bool needs_neighborhood() const { return clarity_ != 0.0f || detail_.any(); }

    // 新 WB 係数 / As Shot WB 係数
    const std::array<float, 3>& wb_ratio() const { return wb_ratio_; }
    // (3) の合成行列: (sRGB→Rec.2020) × rgb_cam
    const Mat3& camera_to_working() const { return camera_to_working_; }

    // GPU 実装用（CPU と同じ値で同じ計算をする）
    float wb_clip() const { return wb_clip_; }
    const float* matrix() const { return m_; }  // 行優先 3×3
    const ToneLut& tone() const { return tone_; }
    bool color_adjust() const { return color_adjust_; }
    float saturation() const { return saturation_; }
    float vibrance() const { return vibrance_; }

    // As Shot / カスタムの WB 係数（G = 1）
    static std::array<double, 3> effective_wb(const Settings& settings, const ColorInfo& color);

private:
    std::array<float, 3> wb_ratio_{1, 1, 1};
    float wb_clip_ = 1.0f;
    Mat3 camera_to_working_;
    float m_[9];
    ToneLut tone_;
    // (5b) 彩度・自然な彩度（どちらも 0 なら何もしない）
    bool color_adjust_ = false;
    float saturation_ = 1.0f;  // 輝度からの距離の倍率
    float vibrance_ = 0.0f;    // -1..1
    float clarity_ = 0.0f;     // -2..2
    DetailParams detail_;
};

// リニア sRGB → リニア Rec.2020
Mat3 srgb_to_rec2020();

} // namespace focal
