#pragma once

#include <vector>

#include "util/thread_pool.h"

namespace focal {

// 5.4 章 (5a) のうち、画素単位の細かい処理: ノイズ低減（輝度・カラー）とシャープネス（明瞭度は local_contrast）。
// トーン適用後・彩度の前のリニア Rec.2020 に対して行う。
//
// 半径はフル解像度の画素で決め、描画の縮尺（出力解像度 / フル解像度）を掛ける。縮小表示では半径が 1 画素未満になり
// ほとんど効かない（原寸を縮めたときに見える分だけ効く）。効き方は 100% 表示と書き出しで確認する。
struct DetailParams {
    float noise_reduction = 0;        // 輝度のノイズ低減 0..1
    float color_noise_reduction = 0;  // カラーのノイズ低減 0..1
    float sharpness = 0;              // シャープネス 0..1.5

    bool any() const { return noise_reduction > 0 || color_noise_reduction > 0 || sharpness > 0; }
};

// 描く範囲の外側に余分に描いておく幅（画素）
int detail_margin(const DetailParams& p, double scale);

// 調整用の定数と、縮尺から決まる半径など（CPU 版と GPU 版で共有する）
namespace detail {
constexpr float kLr = 0.2627f, kLg = 0.6780f, kLb = 0.0593f;  // Rec.2020 の輝度の係数
constexpr float kLumaEpsMax = 0.035f;     // 輝度のノイズ低減で平らにする明暗の幅（√輝度の単位、量 1 のとき）
constexpr float kChromaEps = 1e-3f;       // 色を輝度の輪郭に沿わせる強さ（√輝度の分散）
constexpr float kSharpenThreshold = 0.004f;  // これより小さい細部（ノイズ）は強めない（√輝度の単位）

// 輝度のノイズ低減の窓の半径（0 なら行わない）
int luma_radius(double scale);
// カラーのノイズ低減の窓の半径（0 なら行わない）と、係数を計算する縮小率
int chroma_radius(const DetailParams& p, double scale);
int chroma_factor(int rc);
// シャープネスのガウスぼかしのカーネル（長さ 2r+1、合計 1）。空なら行わない
std::vector<float> sharpen_kernel(const DetailParams& p, double scale);
} // namespace detail

// rgb: w × h のインターリーブ RGB（in-place）。端の外側は端の画素が続いているとみなす。打ち切られたら false
// (origin_x, origin_y) はバッファの左上の、出力画像（この縮尺）での位置。カラーのノイズ低減は縮小して計算するので、
// 縮小の区切りをこれに合わせる（同じ画像のどの範囲を描いても同じ結果にする）
bool apply_noise_reduction(float* rgb, int w, int h, const DetailParams& p, double scale, int origin_x = 0,
                           int origin_y = 0, const CancelToken& cancel = {});
bool apply_sharpen(float* rgb, int w, int h, const DetailParams& p, double scale, const CancelToken& cancel = {});

} // namespace focal
