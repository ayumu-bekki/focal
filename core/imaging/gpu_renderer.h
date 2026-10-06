#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "imaging/color_pipeline.h"
#include "imaging/geometry.h"
#include "imaging/output_transform.h"
#include "imaging/renderer.h"
#include "util/image.h"
#include "util/thread_pool.h"

namespace focal {

// 表示用レンダリング（5.4 章 (1)〜(7)）の GPU 実装の枠組み（ADR-03・ADR-05、v3.14）。
// core は OS に依存しないので、ここでは型だけを決め、実装（Metal）は gpu/ モジュールが持つ。
// Editor に渡されなければ、従来どおり CPU（render_display）で描く。書き出しは常に CPU。
// 結果は render_display と同じ（差は 8-bit で ±2 以内、テストで確認）。

// 描画元。GPU 側は同じ画像を使い回す（weak_ptr で同一性を確かめ、別の画像なら転送し直す）
struct GpuSource {
    std::shared_ptr<const ImageU16> full;  // どちらか一方
    std::shared_ptr<const ImageF> proxy;
    int sensor_w = 0;
    int sensor_h = 0;
    // レンズ補正の補正マップ（v3.27）。GPU 側は格子をテクスチャにして引く
    std::shared_ptr<const LensMaps> lens;

    SourceView view() const { return {full.get(), proxy.get(), sensor_w, sensor_h, lens.get()}; }
};

enum class GpuStatus {
    Ok,
    Cancelled,
    Unsupported,  // この設定は GPU で描けない（CPU で描く）
    Failed,       // GPU のエラー（CPU で描く）
};

class GpuRenderer {
public:
    virtual ~GpuRenderer() = default;
    // GPU の名前（表示・ログ用）
    virtual std::string name() const = 0;
    // render_display と同じ範囲・同じ形式で out に書き、表示用出力のヒストグラムも作る。複数スレッドから呼んでよい
    virtual GpuStatus render(const GpuSource& src, const GeometryPlan& plan, double scale, PointD origin, int w, int h,
                             const ColorPipeline& pipeline, const OutputTransform& transform, uint8_t* out,
                             size_t stride, PixelLayout layout, std::array<std::array<uint32_t, 256>, 3>& histogram,
                             const CancelToken& cancel) = 0;
    // 取っておいた描画元（GPU から見える置き場への写し）を解放する。写真を閉じたときに呼ぶ
    virtual void release_sources() {}
};

} // namespace focal
