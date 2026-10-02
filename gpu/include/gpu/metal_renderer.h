#pragma once

#include <memory>

#include "imaging/gpu_renderer.h"

namespace focal::gpu {

// Metal による表示用レンダラー（v3.14、macOS のみ）。使える GPU がない、FOCAL_GPU=0、
// またはシェーダーのコンパイルに失敗したら nullptr（失敗の内容は標準エラーに出す）
std::unique_ptr<GpuRenderer> create_metal_renderer();

// Metal のデバイスがあるか（テスト用: デバイスがあるのにレンダラーが作れなければ不具合）
bool metal_device_available();

} // namespace focal::gpu
