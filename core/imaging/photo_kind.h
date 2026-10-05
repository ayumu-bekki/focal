#pragma once

#include <optional>
#include <string_view>

namespace focal {

// 写真のファイルの種類（v3.22、design.md 5.12 章）。RAW のほかに、通常の画像ファイルも写真として管理する。
// 数値はカタログ（photos.kind）に保存する
enum class PhotoKind : int { Raw = 0, Jpeg = 1, Tiff = 2, Png = 3, Heif = 4 };

// ファイル名の拡張子（大文字小文字を区別しない）から種類を決める。写真として扱わない拡張子は nullopt。
//   RAW: 3fr arw cr2 … x3f、JPEG: jpg jpeg、TIFF: tif tiff、PNG: png、HEIF: heic heif（OS の読み取り部品が登録されているときだけ）
std::optional<PhotoKind> photo_kind_for_name(std::string_view name);

} // namespace focal
