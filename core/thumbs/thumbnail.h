#pragma once

#include <cstdint>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "edit/settings.h"
#include "util/image.h"

class LibRaw;

namespace focal {

// 10 章のサムネイル。

inline constexpr int kThumbnailLongEdge = 512;

// キャッシュのキー: BLAKE3(正規化パス + file_size + file_mtime [+ Settings のハッシュ])
std::string thumbnail_key(std::string_view nfc_path, int64_t file_size, int64_t file_mtime,
                          std::string_view settings_hash = {});

// Focal の現像結果のキー（10 章: キーに Settings のハッシュを含める）。
// 編集していない写真でも、カメラの埋め込みプレビュー（thumbnail_key）とは別のキーになる。
// 一覧用のサムネイル（ThumbnailCache）と表示用の大きいプレビュー（PreviewCache）で共通
std::string rendered_key(std::string_view nfc_path, int64_t file_size, int64_t file_mtime, const Settings& settings);

// 長辺 512px の JPEG をキーごとに保存するディスクキャッシュ。保存先はアプリが渡す（10 章）。
// 複数スレッドから同時に使ってよい（書き込みは一時ファイル + rename）。
class ThumbnailCache {
public:
    explicit ThumbnailCache(std::filesystem::path dir);

    const std::filesystem::path& dir() const { return dir_; }
    std::filesystem::path path_for(const std::string& key) const;
    bool contains(const std::string& key) const;
    void store(const std::string& key, const ImageU8& image) const;

private:
    std::filesystem::path dir_;
};

// 表示用の大きいプレビューのキャッシュ（現像ビューアで一度表示した写真を、開き直したときにすぐ出す）。
// Display P3 の JPEG（ICC 付き）。上限を超えたら、最後に使った日時が古いものから消す。
// 複数スレッドから同時に使ってよい。
class PreviewCache {
public:
    PreviewCache(std::filesystem::path dir, uint64_t limit_bytes);

    const std::filesystem::path& dir() const { return dir_; }
    bool contains(const std::string& key) const;
    // 見つかれば out に入れて true。使った日時を更新する
    bool load(const std::string& key, ImageU8& out) const;
    void store(const std::string& key, const ImageU8& display_p3);
    void set_limit(uint64_t bytes);
    uint64_t usage() const;
    void clear();
    // 上限を超えていれば古いものから消す
    void enforce_limit();

private:
    std::filesystem::path path_for(const std::string& key) const;

    std::filesystem::path dir_;
    std::atomic<uint64_t> limit_;
    std::atomic<uint64_t> written_since_check_{0};
    std::mutex enforce_mutex_;
};

enum class ThumbnailSource { Embedded, Rendered };

struct Thumbnail {
    ImageU8 image;  // sRGB、向き補正済み
    ThumbnailSource source = ThumbnailSource::Embedded;
};

struct ThumbnailOptions {
    int long_edge = kThumbnailLongEdge;
    bool allow_embedded = true;  // false なら常にレンダリングで作る（テスト用）
    // 編集を反映して描く（指定すれば埋め込みプレビューは使わない）
    std::optional<Settings> settings;
};

// 埋め込みプレビューから作る。使えなければレンダリング（half_size デコード + 既定のパイプライン）で作る。
Thumbnail make_thumbnail(const std::filesystem::path& raw_path, const ThumbnailOptions& options = {});

// open_file 済みの LibRaw から作る（取り込み時にメタデータと同じインスタンスで使う）。
// 埋め込みが使えなければ raw_path を開き直してレンダリングする。
Thumbnail make_thumbnail(LibRaw& raw, const std::filesystem::path& raw_path, const ThumbnailOptions& options = {});

// 埋め込みプレビューの中から使うものを選んで取り出す。
// 埋め込みがない・長辺 256px 未満・JPEG でない場合は nullopt（10 章のフォールバック条件）。
std::optional<ImageU8> extract_embedded_preview(LibRaw& raw, int long_edge);

// LibRaw の flip 値に従って向きを補正する（flip_index と同じ対応）
ImageU8 apply_orientation(const ImageU8& src, int flip);

// 長辺が long_edge になるよう面積平均で縮小する（縮小不要ならそのまま）
ImageU8 downscale_u8(const ImageU8& src, int long_edge);

} // namespace focal
