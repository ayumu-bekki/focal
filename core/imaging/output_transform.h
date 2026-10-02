#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace focal {

// 5.4 (6)(7): 作業色空間（リニア Rec.2020）→ 出力色空間 + 量子化。lcms2 で行う。
enum class OutputSpace {
    Srgb,       // 書き出し用
    DisplayP3,  // macOS の表示用（ディスプレイへの変換は OS が行う、8 章）
};

enum class OutputDepth { U8, U16 };

class OutputTransform {
public:
    OutputTransform(OutputSpace space, OutputDepth depth);
    ~OutputTransform();
    OutputTransform(const OutputTransform&) = delete;
    OutputTransform& operator=(const OutputTransform&) = delete;

    OutputSpace space() const { return space_; }
    OutputDepth depth() const { return depth_; }

    // rgb: リニア Rec.2020 の float（インターリーブ）。out: U8 なら uint8_t、U16 なら uint16_t。
    // 複数スレッドから同時に呼んでよい。
    // U8 は lcms2 から取り出した行列と TRC の表で直接計算する高速経路（表示の 16ms 予算のため）。
    // 両プロファイルとも matrix-shaper なので、lcms2 の変換と同じ結果になる（テストで ±1 以内を確認）。
    void apply(const float* rgb, void* out, size_t pixels) const;

    // U8 のみ: B, G, R, 255 の 4 バイトで書く（Core Animation がそのまま使える 32-bit 形式。macOS の表示用）
    void apply_bgrx(const float* rgb, uint8_t* out, size_t pixels) const;

    // 常に lcms2 の cmsDoTransform で変換する（基準実装・テスト用）
    void apply_lcms(const float* rgb, void* out, size_t pixels) const;

    // 出力色空間の ICC プロファイル（埋め込み用）
    std::vector<uint8_t> icc_profile() const;

    // GPU 実装用: U8 高速経路の行列（行優先 3×3）と TRC 表（65536 要素）
    const float* fast_matrix() const { return matrix_; }
    const std::vector<uint8_t>& fast_trc() const { return trc_u8_; }

private:
    OutputSpace space_;
    OutputDepth depth_;
    void* transform_ = nullptr;  // cmsHTRANSFORM
    void* out_profile_ = nullptr;  // cmsHPROFILE

    // U8 高速経路: 作業色空間 → 出力原色のリニア値の行列と、リニア値（0..1 を 65535 分割）→ 8-bit の TRC 表
    float matrix_[9] = {};
    std::vector<uint8_t> trc_u8_;
};

} // namespace focal
