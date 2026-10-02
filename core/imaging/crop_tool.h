#pragma once

#include "edit/settings.h"
#include "imaging/geometry.h"

namespace focal {

// クロップモードの操作（5.6 章、9.1 章）。UI 層に幾何の計算を置かないため core に置く（ADR-10）。
// クロップ枠はキャンバス（(c) 傾き補正後、canvas_w × canvas_h 画素）上の正規化矩形で扱う。

enum class CropHandle { Move, Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };

// 枠の四隅が、傾き補正後の画像（余白でない部分）に入っているか
bool crop_inside_image(const CropRect& c, double straighten_deg, double canvas_w, double canvas_h);

// 縦横比 aspect（幅 / 高さ、画素で。0 なら今の枠の比）を保ち、画像に収まる最大の枠。
// 中心は center（正規化）に置き、収まらなければキャンバスの中央に寄せる。
// 傾き補正や縦横比を変えたときの自動調整に使う
CropRect max_crop(double aspect, double straighten_deg, double canvas_w, double canvas_h,
                  PointD center = {0.5, 0.5});

// ハンドルのドラッグ。start はドラッグ開始時の枠、dx / dy はキャンバスの正規化座標での移動量。
// 反対側の辺（角）を固定し、縦横比（aspect > 0 なら）を保ち、画像からはみ出す分は start に向かって縮める
CropRect drag_crop(const CropRect& start, CropHandle handle, double dx, double dy, double aspect,
                   double straighten_deg, double canvas_w, double canvas_h);

// 時計回りに 90° × steps 回したときのクロップ枠（画像と一緒に回す）
CropRect rotate_crop(const CropRect& c, int steps);

// 水平線ツール: キャンバス上の 2 点を結ぶ線が水平（縦に近い線なら垂直）になる傾き補正値。
// 今の傾き補正値に加えて返す（-45..45 に収める）
double straighten_from_line(PointD a, PointD b, double current_deg);

// 縦横比のモードを画素の比にする。Free なら 0（制約なし）
double crop_aspect(AspectMode mode, double canvas_w, double canvas_h);

} // namespace focal
