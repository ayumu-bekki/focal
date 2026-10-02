#pragma once

#include <array>

#include "imaging/raw_decoder.h"

namespace focal {

// 色温度・色かぶり ⇔ WB 係数（6.2 章）。
// 黒体軌跡と等色温度線は Robertson の表（CIE 1960 uv）で求める。
// tint の尺度は DNG SDK と同じ（uv 上の距離 × -3000、正でマゼンタ寄り）。

struct ChromaXy {
    double x = 0, y = 0;
};

ChromaXy temp_tint_to_xy(double temperature, double tint);
void xy_to_temp_tint(ChromaXy xy, double& temperature, double& tint);

// K / tint の光源下で白をニュートラルにするカメラの WB 係数（G = 1）。
std::array<double, 3> wb_from_temp_tint(double temperature, double tint, const ColorInfo& color);

// WB 係数から K / tint を求める（As Shot のスライダー初期値用）。
void temp_tint_from_wb(const std::array<double, 3>& wb, const ColorInfo& color, double& temperature,
                       double& tint);

} // namespace focal
