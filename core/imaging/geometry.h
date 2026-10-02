#pragma once

#include "edit/settings.h"

namespace focal {

// 5.6 章のジオメトリ。
// 座標はすべて連続座標（画素 (i, j) の中心が (i+0.5, j+0.5)）。
//   センサー → (a) flip → (b) rotate90 → (c) straighten → (d) crop = 出力

struct PointD {
    double x = 0, y = 0;
};

// x' = a*x + b*y + c,  y' = d*x + e*y + f
struct Affine {
    double a = 1, b = 0, c = 0, d = 0, e = 1, f = 0;

    PointD apply(PointD p) const { return {a * p.x + b * p.y + c, d * p.x + e * p.y + f}; }
    // (*this) ∘ inner : まず inner を適用し、次に this を適用する
    Affine after(const Affine& inner) const;
    Affine inverse() const;

    static Affine translate(double tx, double ty) { return {1, 0, tx, 0, 1, ty}; }
    static Affine scale(double sx, double sy) { return {sx, 0, 0, 0, sy, 0}; }
};

class GeometryPlan {
public:
    // sensor_w/h: フル解像度のセンサー座標でのサイズ（向き未適用）
    GeometryPlan(int sensor_w, int sensor_h, int flip, const GeometrySettings& g, bool apply_crop = true);

    // (a)(b) 適用後のサイズ（= (c) のキャンバスサイズ）
    double canvas_width() const { return w2_; }
    double canvas_height() const { return h2_; }

    // フル解像度での出力サイズ（クロップ後、実数）
    double output_width() const { return crop_w_; }
    double output_height() const { return crop_h_; }

    // 出力画素座標 → センサー画素座標の逆写像。
    // scale: 出力 1 画素あたりのフル解像度画素数の逆数（= 出力解像度 / フル解像度）
    // origin: 出力全体のうち、描画する領域の左上（出力画素座標）。100% 表示の部分描画用
    Affine output_to_sensor(double scale, PointD origin = {}) const;

    // (c) キャンバス座標 → センサー座標
    const Affine& canvas_to_sensor() const { return canvas_to_sensor_; }

private:
    double w2_ = 0, h2_ = 0;
    double crop_x_ = 0, crop_y_ = 0, crop_w_ = 0, crop_h_ = 0;
    Affine canvas_to_sensor_;
};

// (a)(b) 適用後のサイズ
void oriented_size(int sensor_w, int sensor_h, int flip, int rotate90, int& out_w, int& out_h);

// 傾き補正で四隅に余白が出ないよう、クロップ枠を中心と縦横比を保ったまま縮める（5.6 章）。
// canvas_w/h は (c) のキャンバスサイズ（画素）。
CropRect fit_crop_to_straighten(const CropRect& crop, double straighten_deg, double canvas_w, double canvas_h);

// 縦横比モードの比（幅/高さ）。Free は 0 を返す。canvas は (c) のキャンバスサイズ。
double aspect_ratio(AspectMode mode, double canvas_w, double canvas_h);

} // namespace focal
