#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace focal {

// パス ⇔ UTF-8 文字列。Windows でも UTF-8 として扱う（ワイド文字 API への変換は path が行う）。
std::string path_to_utf8(const std::filesystem::path& p);
std::filesystem::path utf8_to_path(std::string_view s);

// fopen のワイド文字対応版
FILE* open_file(const std::filesystem::path& path, const char* mode);

struct FileStat {
    int64_t size = 0;
    int64_t mtime = 0;  // UNIX 秒
};

// 通常ファイルの情報。存在しない・通常ファイルでなければ nullopt
std::optional<FileStat> stat_file(const std::filesystem::path& path);

// dir の中で、NFC にすると name_nfc と一致するエントリを探す。
// 正規化を区別するファイルシステム（Linux の ext4 など）で、NFC で保存した名前から実際のファイルを開くために使う。
std::optional<std::filesystem::path> find_entry_nfc(const std::filesystem::path& dir, std::string_view name_nfc);

// NFC で保存した絶対パスから、実際に開けるパスを求める。
// そのまま存在すればそれを返し、なければ先頭から 1 要素ずつ find_entry_nfc で解決する。
std::optional<std::filesystem::path> resolve_nfc_path(std::string_view abs_nfc);

// 各ディレクトリ（NFC で保存した絶対パス）にいま到達できるか。ネットワークボリュームが応答しないときに待ち続けないよう、
// 全体で timeout まで待ち、間に合わなかったものは false（到達できない）にする。確認は別スレッドで行う（v3.19）
std::vector<bool> directories_reachable(const std::vector<std::string>& abs_nfc_paths, std::chrono::milliseconds timeout);

// 絶対パスにして正規化し、区切りを '/' にした NFC の文字列（カタログに保存する形）
std::string normalized_path_string(const std::filesystem::path& p);

} // namespace focal
