#include "imaging/geometry.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace focal {

Affine Affine::after(const Affine& in) const {
    return {a * in.a + b * in.d, a * in.b + b * in.e, a * in.c + b * in.f + c,
            d * in.a + e * in.d, d * in.b + e * in.e, d * in.c + e * in.f + f};
}

Affine Affine::inverse() const {
    const double det = a * e - b * d;
    const double ia = e / det, ib = -b / det, id = -d / det, ie = a / det;
    return {ia, ib, -(ia * c + ib * f), id, ie, -(id * c + ie * f)};
}

namespace {

// (a) 向き補正後の座標 → センサー座標。LibRaw の flip_index と同じ対応。
Affine oriented_to_sensor(int sw, int sh, int flip) {
    Affine t;
    if (flip & 4) t = Affine{0, 1, 0, 1, 0, 0};  // 転置
    if (flip & 2) t = Affine{1, 0, 0, 0, -1, static_cast<double>(sh)}.after(t);
    if (flip & 1) t = Affine{-1, 0, static_cast<double>(sw), 0, 1, 0}.after(t);
    return t;
}

// (c) 傾き補正後（キャンバス）座標 → 補正前。正の角度で画像を時計回りに回す。
Affine straightened_to_rotated(double w, double h, double deg) {
    const double rad = deg * std::numbers::pi / 180.0;
    const double cs = std::cos(rad), sn = std::sin(rad);
    const double cx = w / 2, cy = h / 2;
    // 逆回転 R(-θ) を中心まわりに
    const Affine r{cs, sn, 0, -sn, cs, 0};
    return Affine::translate(cx, cy).after(r).after(Affine::translate(-cx, -cy));
}

} // namespace

void oriented_size(int sensor_w, int sensor_h, int flip, int rotate90, int& out_w, int& out_h) {
    out_w = (flip & 4) ? sensor_h : sensor_w;
    out_h = (flip & 4) ? sensor_w : sensor_h;
    if (rotate90 & 1) std::swap(out_w, out_h);
}

GeometryPlan::GeometryPlan(int sensor_w, int sensor_h, int flip, const GeometrySettings& g, bool apply_crop) {
    const double w1 = (flip & 4) ? sensor_h : sensor_w;
    const double h1 = (flip & 4) ? sensor_w : sensor_h;
    const int k = ((g.rotate90 % 4) + 4) % 4;
    w2_ = (k & 1) ? h1 : w1;
    h2_ = (k & 1) ? w1 : h1;

    // (b) 回転後 → 回転前。i 段目（高さ h）を時計回りに回した i+1 段目の座標 (x, y) は、
    // i 段目の (y, h - x) に対応する。k 段目 → 0 段目になるよう内側に合成していく
    Affine rot;
    double h = h1;
    for (int i = 0; i < k; ++i) {
        rot = rot.after(Affine{0, 1, 0, -1, 0, h});
        h = (i % 2 == 0) ? w1 : h1;
    }

    canvas_to_sensor_ = oriented_to_sensor(sensor_w, sensor_h, flip)
                            .after(rot)
                            .after(straightened_to_rotated(w2_, h2_, g.straighten));

    if (apply_crop) {
        crop_x_ = g.crop.x * w2_;
        crop_y_ = g.crop.y * h2_;
        crop_w_ = g.crop.w * w2_;
        crop_h_ = g.crop.h * h2_;
    } else {
        crop_w_ = w2_;
        crop_h_ = h2_;
    }
}

Affine GeometryPlan::output_to_sensor(double scale, PointD origin) const {
    // 出力画素 → キャンバス: (u + origin) / scale + crop 左上
    const Affine out_to_canvas{1.0 / scale, 0, origin.x / scale + crop_x_, 0, 1.0 / scale, origin.y / scale + crop_y_};
    return canvas_to_sensor_.after(out_to_canvas);
}

CropRect fit_crop_to_straighten(const CropRect& crop, double straighten_deg, double cw, double ch) {
    if (std::abs(straighten_deg) < 1e-9) return crop;
    const Affine to_rot = straightened_to_rotated(cw, ch, straighten_deg);
    auto inside = [&](double x, double y) {
        const PointD p = to_rot.apply({x, y});
        constexpr double eps = 1e-9;
        return p.x >= -eps && p.y >= -eps && p.x <= cw + eps && p.y <= ch + eps;
    };

    double cx = (crop.x + crop.w / 2) * cw;
    double cy = (crop.y + crop.h / 2) * ch;
    if (!inside(cx, cy)) {
        cx = cw / 2;
        cy = ch / 2;
    }
    const double hw = crop.w * cw / 2, hh = crop.h * ch / 2;
    auto fits = [&](double s) {
        return inside(cx - hw * s, cy - hh * s) && inside(cx + hw * s, cy - hh * s) &&
               inside(cx - hw * s, cy + hh * s) && inside(cx + hw * s, cy + hh * s);
    };
    double lo = 0.0, hi = 1.0;
    if (!fits(1.0)) {
        for (int i = 0; i < 60; ++i) {
            const double mid = (lo + hi) / 2;
            (fits(mid) ? lo : hi) = mid;
        }
    } else {
        lo = 1.0;
    }
    CropRect r;
    r.w = crop.w * lo;
    r.h = crop.h * lo;
    r.x = cx / cw - r.w / 2;
    r.y = cy / ch - r.h / 2;
    return r;
}

double aspect_ratio(AspectMode mode, double cw, double ch) {
    switch (mode) {
    case AspectMode::Free: return 0.0;
    case AspectMode::Original: return cw / ch;
    case AspectMode::R1x1: return 1.0;
    case AspectMode::R3x2: return cw >= ch ? 3.0 / 2.0 : 2.0 / 3.0;
    case AspectMode::R4x3: return cw >= ch ? 4.0 / 3.0 : 3.0 / 4.0;
    case AspectMode::R16x9: return cw >= ch ? 16.0 / 9.0 : 9.0 / 16.0;
    case AspectMode::R5x4: return cw >= ch ? 5.0 / 4.0 : 4.0 / 5.0;
    }
    return 0.0;
}

} // namespace focal
