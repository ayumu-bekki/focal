#pragma once

#include "edit/settings.h"
#include "imaging/color_pipeline.h"
#include "imaging/geometry.h"
#include "imaging/output_transform.h"
#include "imaging/raw_decoder.h"
#include "util/image.h"
#include "util/thread_pool.h"

namespace focal {

enum class Interpolation { Bilinear, Bicubic };

// 表示用出力の画素形式
enum class PixelLayout {
    Rgb8,   // R, G, B
    Bgrx8,  // B, G, R, 255（macOS の Core Animation がそのまま使う形式）
};

// レンダリングのソース。フル解像度（uint16）かプロキシ（float）のどちらか。
struct SourceView {
    const ImageU16* full = nullptr;
    const ImageF* proxy = nullptr;
    // ジオメトリの基準になるセンサー座標のサイズ（= フル解像度バッファのサイズ）
    int sensor_w = 0;
    int sensor_h = 0;

    int width() const { return full ? full->width : proxy->width; }
    int height() const { return full ? full->height : proxy->height; }
};

// 出力画素座標 → ソース（フル解像度かプロキシ）の画素座標
Affine output_to_source(const SourceView& src, const GeometryPlan& plan, double scale, PointD origin);

// 周辺画素を使う処理（5a）のために余分に描く範囲（CPU 版と GPU 版で共有する）。
// 描く範囲 (origin, w, h) の左右上下に足す画素数（出力画像の範囲内だけ）と、明瞭度のぼかしの σ
struct NeighborhoodRegion {
    double sigma = 0;  // 明瞭度の σ（この縮尺の画素）
    int ox = 0, oy = 0;  // floor(origin)
    int left = 0, top = 0, right = 0, bottom = 0;
    int bw = 0, bh = 0;  // 余白込みの大きさ
};
NeighborhoodRegion plan_neighborhood(const SourceView& src, const GeometryPlan& plan, double scale, PointD origin,
                                     int w, int h, const ColorPipeline& pipeline);

// 出力画像のうち (origin, w, h) の領域を、5.4 章の (1)〜(5b) まで処理してリニア Rec.2020 で返す。
// 周辺画素を使う処理（5a: ノイズ低減・明瞭度・シャープネス）があれば、領域の外側も余分に描いてからかける。
// scale は「出力解像度 / フル解像度」。打ち切られたら false を返す。
bool render_linear(const SourceView& src, const GeometryPlan& plan, double scale, PointD origin, int w, int h,
                   const ColorPipeline& pipeline, Interpolation interp, float* out,
                   const CancelToken& cancel = {});

// (1)〜(7) をまとめて行う（プレビュー・100% 表示用）。行ブロック単位で並列化する。
bool render_display(const SourceView& src, const GeometryPlan& plan, double scale, PointD origin, int w, int h,
                    const ColorPipeline& pipeline, const OutputTransform& transform, Interpolation interp,
                    uint8_t* out, size_t stride, const CancelToken& cancel = {},
                    PixelLayout layout = PixelLayout::Rgb8);

// 書き出し用: フル解像度からバイキュービックで描き、必要なら Lanczos3 で長辺を縮める（5.8 章）。
// 戻り値はリニア Rec.2020（(6) の前）。
ImageF render_for_export(const DecodedRaw& raw, const Settings& settings, int long_edge);

// プロキシからのプレビュー描画。出力サイズはクロップ後の画像をプロキシの縮尺で描いたもの。
ImageU8 render_preview(const DecodedRaw& raw, const ImageF& proxy, const Settings& settings,
                       const OutputTransform& transform);

// リニア Rec.2020 → 出力色空間（量子化込み）
ImageU8 encode_u8(const ImageF& linear, const OutputTransform& transform);
ImageU16 encode_u16(const ImageF& linear, const OutputTransform& transform);

} // namespace focal
