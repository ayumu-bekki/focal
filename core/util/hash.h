#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace focal {

// BLAKE3 の 256-bit ダイジェストを 16 進文字列（64 文字）で返す。
std::string blake3_hex(std::string_view data);

// 7.2 章の quick_hash: BLAKE3(サイズ + 先頭 1MB + 末尾 1MB)。
// サイズは 8 バイトのリトルエンディアン。2MB 以下のファイルは重ならないよう全体を 1 回だけ読む。
std::string quick_hash(const std::filesystem::path& path);

} // namespace focal
