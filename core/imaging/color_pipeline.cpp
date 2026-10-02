#include "imaging/color_pipeline.h"

#include <algorithm>

#include "imaging/white_balance.h"
#include "util/error.h"

namespace focal {

namespace {

ToneParams tone_params(const Settings& s) {
    return {s.exposure, s.contrast, s.highlights, s.shadows, s.whites, s.blacks, s.brightness};
}

// Rec.2020 の輝度の係数
constexpr float kLr = 0.2627f, kLg = 0.6780f, kLb = 0.0593f;

// 肌色（赤〜橙、HSV の色相で約 10°〜45°）の重み 0..1
inline float skin_weight(float r, float g, float b, float mx, float mn) {
    if (mx != r || g < b || mx - mn < 1e-6f) return 0.0f;  // 赤〜黄の範囲外、またはグレー
    const float h = (g - b) / (mx - mn);  // 0..1 が 0°〜60°
    const float t = 1.0f - std::abs(h - 0.45f) / 0.3f;
    return std::clamp(t, 0.0f, 1.0f);
}

} // namespace

Mat3 srgb_to_rec2020() {
    const Mat3 srgb = rgb_to_xyz_matrix(primaries::kSrgb, primaries::kD65);
    const Mat3 rec2020 = rgb_to_xyz_matrix(primaries::kRec2020, primaries::kD65);
    return rec2020.inverse() * srgb;
}

std::array<double, 3> ColorPipeline::effective_wb(const Settings& s, const ColorInfo& color) {
    if (s.wb.mode == WhiteBalanceSettings::Mode::Custom)
        return wb_from_temp_tint(s.wb.temperature, s.wb.tint, color);
    return color.as_shot_wb;
}

ColorPipeline::ColorPipeline(const Settings& settings, const ColorInfo& color) : tone_(tone_params(settings)) {
    if (settings.process_version != 1)
        throw Error(Error::Code::Unsupported,
                    "unsupported processVersion " + std::to_string(settings.process_version));

    const auto wb = effective_wb(settings, color);
    for (int c = 0; c < 3; ++c) wb_ratio_[c] = static_cast<float>(wb[c] / color.as_shot_wb[c]);
    // 飽和した画素が色付かないよう、最初に飽和するチャンネルの値で全チャンネルを頭打ちにする
    wb_clip_ = *std::min_element(wb_ratio_.begin(), wb_ratio_.end());

    saturation_ = static_cast<float>(1.0 + settings.saturation / 100.0);
    vibrance_ = static_cast<float>(settings.vibrance / 100.0);
    color_adjust_ = settings.saturation != 0.0 || settings.vibrance != 0.0;
    clarity_ = static_cast<float>(settings.clarity / 100.0);
    detail_.noise_reduction = static_cast<float>(settings.noise_reduction / 100.0);
    detail_.color_noise_reduction = static_cast<float>(settings.color_noise_reduction / 100.0);
    detail_.sharpness = static_cast<float>(settings.sharpness / 100.0);

    camera_to_working_ = srgb_to_rec2020() * color.rgb_cam;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) m_[r * 3 + c] = static_cast<float>(camera_to_working_.m[r][c]);
}

void ColorPipeline::process(std::span<float> rgb) const {
    process_tone(rgb);
    process_color(rgb);
}

void ColorPipeline::process_tone(std::span<float> rgb) const {
    const float wr = wb_ratio_[0], wg = wb_ratio_[1], wbb = wb_ratio_[2], clip = wb_clip_;
    const size_t n = rgb.size() / 3;
    float* p = rgb.data();
    for (size_t i = 0; i < n; ++i, p += 3) {
        // (2) WB
        const float r = std::min(p[0] * wr, clip);
        const float g = std::min(p[1] * wg, clip);
        const float b = std::min(p[2] * wbb, clip);
        // (3) カメラ RGB → リニア Rec.2020（負値はここではクリップしない）
        const float x = m_[0] * r + m_[1] * g + m_[2] * b;
        const float y = m_[3] * r + m_[4] * g + m_[5] * b;
        const float z = m_[6] * r + m_[7] * g + m_[8] * b;
        // (4)(5) 露出 + トーン（LUT に合成済み）
        p[0] = tone_.apply(x);
        p[1] = tone_.apply(y);
        p[2] = tone_.apply(z);
    }
}

void ColorPipeline::process_color(std::span<float> rgb) const {
    if (!color_adjust_) return;
    // (5b) 彩度・自然な彩度: 輝度を保って、輝度からの距離を伸び縮みさせる
    const size_t n = rgb.size() / 3;
    float* p = rgb.data();
    for (size_t i = 0; i < n; ++i, p += 3) {
        const float r = p[0], g = p[1], b = p[2];
        const float l = kLr * r + kLg * g + kLb * b;
        float k = saturation_;
        if (vibrance_ != 0.0f) {
            const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
            if (mx > 1e-6f) {
                const float chroma = std::clamp((mx - mn) / mx, 0.0f, 1.0f);  // 今の鮮やかさ
                float v = vibrance_ * (1.0f - chroma) * (1.0f - chroma);
                if (vibrance_ > 0.0f) v *= 1.0f - 0.6f * skin_weight(r, g, b, mx, mn);
                k *= 1.0f + v;
            }
        }
        p[0] = std::max(0.0f, l + (r - l) * k);
        p[1] = std::max(0.0f, l + (g - l) * k);
        p[2] = std::max(0.0f, l + (b - l) * k);
    }
}

} // namespace focal
