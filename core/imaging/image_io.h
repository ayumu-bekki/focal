#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "util/image.h"

namespace focal {

enum class TiffCompression { None, Deflate };

// 16-bit / 8-bit RGB の TIFF を書く。icc が空でなければ埋め込む。
void write_tiff(const std::filesystem::path& path, const ImageU16& image, const std::vector<uint8_t>& icc,
                TiffCompression compression = TiffCompression::None);
void write_tiff(const std::filesystem::path& path, const ImageU8& image, const std::vector<uint8_t>& icc,
                TiffCompression compression = TiffCompression::None);

// 8-bit RGB の JPEG を書く。icc が空でなければ埋め込む。
void write_jpeg(const std::filesystem::path& path, const ImageU8& image, int quality,
                const std::vector<uint8_t>& icc);

// 読み戻し（テスト・比較用）。8-bit は 16-bit に拡張せず、bits で区別する。
struct LoadedImage {
    int width = 0;
    int height = 0;
    int bits = 0;
    std::vector<uint16_t> data;  // RGB インターリーブ。8-bit の場合も値は 0..255
    std::vector<uint8_t> icc;
};

LoadedImage read_tiff(const std::filesystem::path& path);

// メモリ上の JPEG を RGB 8-bit で読む。min_long_edge > 0 なら、長辺がそれを下回らない範囲で
// libjpeg の縮小デコード（1/2, 1/4, 1/8）を使って速く読む。
ImageU8 decode_jpeg(const uint8_t* data, size_t size, int min_long_edge = 0);
LoadedImage read_jpeg(const std::filesystem::path& path);

} // namespace focal
