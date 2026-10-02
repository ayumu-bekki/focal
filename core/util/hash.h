#pragma once

#include <atomic>
#include <cstddef>
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

// ファイルを少しずつ読みながらハッシュを作る（BLAKE3、256-bit、16 進 64 文字）。取り込みのコピー検証に使う（v3.19）
class Blake3Stream {
public:
    Blake3Stream();
    ~Blake3Stream();
    Blake3Stream(const Blake3Stream&) = delete;
    Blake3Stream& operator=(const Blake3Stream&) = delete;

    void update(const void* data, size_t size);
    std::string finish_hex();

private:
    struct Impl;
    Impl* impl_;
};

// ファイル全体の BLAKE3。読めなければ Error(Io)。cancel が true になったら Error(Cancelled)
std::string blake3_file_hex(const std::filesystem::path& path, const std::atomic<bool>* cancel = nullptr);

} // namespace focal
