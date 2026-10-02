#include "imaging/tone_curve.h"

#include <algorithm>
#include <cmath>

namespace focal {

namespace {

constexpr double kMidGray = 0.18;

double smoothstep(double e0, double e1, double x) {
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// ベースカーブのパラメータ（process_version 1 で固定）
constexpr double kGain = 1.25;        // 中間調の持ち上げ
constexpr double kToe = 0.005;        // つま先の強さ
constexpr double kShoulderAt = 0.40;  // 肩の開始点（入力）

double toe(double x) { return kGain * x * x / (x + kToe); }
double toe_slope(double x) { return kGain * (x * x + 2.0 * x * kToe) / ((x + kToe) * (x + kToe)); }

} // namespace

double base_curve(double x) {
    if (x <= 0.0) return 0.0;
    if (x < kShoulderAt) return toe(x);
    // 肩: 値と傾きが連続になる指数関数で 1 に漸近させる
    static const double yt = toe(kShoulderAt);
    static const double st = toe_slope(kShoulderAt);
    return yt + (1.0 - yt) * (1.0 - std::exp(-st * (x - kShoulderAt) / (1.0 - yt)));
}

double tone_reference(double x, const ToneParams& p) {
    x *= std::exp2(p.exposure);
    if (x <= 0.0) return 0.0;
    double e = std::log2(x / kMidGray);
    e *= 1.0 + 0.5 * p.contrast / 100.0;
    e += 1.5 * (p.highlights / 100.0) * smoothstep(0.0, 3.0, e);
    e += 1.5 * (p.shadows / 100.0) * smoothstep(0.0, 4.0, -e);
    // 白・黒: ハイライト・シャドウより外側（ベースカーブの肩とつま先）だけに効く
    e += 2.0 * (p.whites / 100.0) * smoothstep(0.5, 4.0, e);
    e += 2.0 * (p.blacks / 100.0) * smoothstep(1.5, 5.5, -e);
    double y = std::clamp(base_curve(kMidGray * std::exp2(e)), 0.0, 1.0);
    // 明るさ: 0 と 1 を動かさずに中間調を持ち上げる（+100 で指数 1/2.0、-100 で 2.0）
    if (p.brightness != 0.0) y = std::pow(y, std::exp2(-p.brightness / 100.0));
    return y;
}

ToneLut::ToneLut(const ToneParams& p) : lut_(kSize), min_x_(std::exp2(kMinLog2)) {
    for (int i = 0; i < kSize; ++i) {
        const double l = kMinLog2 + (kMaxLog2 - kMinLog2) * i / (kSize - 1);
        lut_[i] = static_cast<float>(tone_reference(std::exp2(l), p));
    }
}

float ToneLut::apply(float x) const {
    if (!(x > min_x_)) return x > 0.0f ? lut_[0] * (x / min_x_) : 0.0f;
    const float pos = (std::log2(x) - kMinLog2) * ((kSize - 1) / (kMaxLog2 - kMinLog2));
    if (pos >= kSize - 1) return lut_[kSize - 1];
    const int i = static_cast<int>(pos);
    const float f = pos - i;
    return lut_[i] + (lut_[i + 1] - lut_[i]) * f;
}

} // namespace focal
