#pragma once

#include <vector>

namespace focal {

// 露出・コントラスト・ハイライト・シャドウ・白・黒・ベースカーブ・明るさを合成した 1D トーン（5.4 (4)(5), 5.5 章）。
// 入力はリニア値（露出前）、出力は 0..1 のリニア値。
struct ToneParams {
    double exposure = 0.0;  // EV
    double contrast = 0.0;  // -100..100
    double highlights = 0.0;
    double shadows = 0.0;
    double whites = 0.0;      // -100..100
    double blacks = 0.0;      // -100..100
    double brightness = 0.0;  // -100..100（ベースカーブの後、0 と 1 を動かさないべき乗）
};

// LUT を使わずに 1 点を厳密に計算する（LUT 生成とテスト用）
double tone_reference(double x, const ToneParams& p);

// ベースカーブ単体（肩とつま先を持つ S 字、入力 0〜約16 → 出力 0〜1）
double base_curve(double x);

class ToneLut {
public:
    static constexpr int kSize = 4096;
    static constexpr float kMinLog2 = -20.0f;  // これより暗い入力は 0 へ線形に落とす
    static constexpr float kMaxLog2 = 6.0f;    // これより明るい入力は最後の値で飽和

    explicit ToneLut(const ToneParams& p);

    float apply(float x) const;

    // GPU 実装用（同じ表と同じ計算をする）
    const std::vector<float>& table() const { return lut_; }
    float min_x() const { return min_x_; }

private:
    std::vector<float> lut_;
    float min_x_;
};

} // namespace focal
