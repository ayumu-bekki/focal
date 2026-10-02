#pragma once

#include "util/thread_pool.h"

namespace focal {

// 明瞭度（局所コントラスト、5.4 章 (5a)）。トーン適用後・彩度の前のリニア Rec.2020 に対して行う。
// 輝度の対数をガウスぼかしした値との差（局所的な明暗）を中間調中心に強め、各チャンネルに同じ比率で掛ける。
//
// ぼかしの σ はセンサーの長辺 × kClaritySigmaFraction を、描画の縮尺（出力解像度 / フル解像度）に合わせたもの。
// これでプロキシ・100% 表示・書き出し・サムネイルの効き方がそろう。
constexpr double kClaritySigmaFraction = 0.01;

// 描く範囲の外側に余分に描いておく幅（画素）。これより外の画素はぼかしにほとんど影響しない
int local_contrast_margin(double sigma);

// 調整用の定数（CPU 版と GPU 版で共有する）
namespace local_contrast {
constexpr float kMinLum = 1e-5f;
constexpr float kMidLog = -2.4739f;  // log2(0.18)
constexpr float kMidWidth = 2.0f;    // 中間調の重みの幅（EV）
constexpr float kEdgeEps = 0.1f;     // これより分散の大きい（約 0.3 EV を超える）明暗の境界はぼかさない
constexpr float kStrength = 1.5f;    // 明瞭度 100 のときの、局所的な明暗（EV）の倍率 + 1
} // namespace local_contrast

// 縮小して計算するときの区切り（CPU 版と GPU 版で共有する）。
// 縮小率 f、縮小画像の左上の区切り番号 (gx0, gy0)、縮小画像の大きさ (sw, sh)、縮小画像上の箱型ぼかし（3 回）の半径 r
struct LocalContrastPlan {
    int f = 1, gx0 = 0, gy0 = 0, sw = 0, sh = 0, r = 0;
};
LocalContrastPlan plan_local_contrast(double sigma, int origin_x, int origin_y, int w, int h);

// rgb: w × h のインターリーブ RGB（in-place）。amount は -2..2（明瞭度 / 100）。
// (origin_x, origin_y) はバッファの左上の、出力画像（この縮尺）での位置。内部の縮小の区切りをこれに合わせるので、
// 同じ画像のどの範囲を描いても同じ結果になる（100% 表示の範囲を変えても値が揺れない）。
// 端の外側は端の画素が続いているとみなす。打ち切られたら false
bool apply_local_contrast(float* rgb, int w, int h, float amount, double sigma, int origin_x = 0, int origin_y = 0,
                          const CancelToken& cancel = {});

} // namespace focal
