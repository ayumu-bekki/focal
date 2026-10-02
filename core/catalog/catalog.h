#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace focal {

class ThumbnailCache;

namespace db {
class Database;
class DbWriter;
} // namespace db

// 7 章のカタログ。書き込みは専用スレッド（DbWriter）、読み取りは呼び出し側スレッドの読み取り専用接続で行う。
// パス・ファイル名・タグ名は NFC で保存する（7.1 章）。元ファイルには一切書き込まない（ADR-12）。

struct RootInfo {
    int64_t id = 0;
    std::string path;  // 絶対パス（NFC、区切りは '/'）
    std::string label;
};

struct FolderInfo {
    int64_t id = 0;
    int64_t root_id = 0;
    std::optional<int64_t> parent_id;
    std::string rel_path;  // ルートからの相対パス（ルート自身は ""）
    int64_t photo_count = 0;
};

struct TagInfo {
    int64_t id = 0;
    std::optional<int64_t> parent_id;
    std::string name;
    std::string path;  // "親/子" 形式
    int64_t photo_count = 0;
};

enum class PhotoStatus { Ok = 0, Missing = 1, Unsupported = 2 };

struct AlbumInfo {
    int64_t id = 0;
    std::string name;
    int64_t photo_count = 0;
};

struct PhotoRecord {
    int64_t id = 0;
    int64_t folder_id = 0;
    std::string file_name;
    int64_t file_size = 0;
    int64_t file_mtime = 0;
    std::string quick_hash;
    PhotoStatus status = PhotoStatus::Ok;
    std::optional<std::string> capture_time;
    std::string camera_make;
    std::string camera_model;
    std::string lens_model;
    std::optional<int64_t> iso;
    std::optional<double> exposure_time;
    std::optional<double> f_number;
    std::optional<double> focal_length;
    int width = 0;
    int height = 0;
    int orientation = 0;
    int rating = 0;
    int flag = 0;  // -1: リジェクト、0: なし、1: ピック
    std::string path;  // 絶対パス（NFC）。ディスク上の実名とは正規化が違うことがある（photo_disk_path を使う）
};

enum class FlagFilter { Any, Picked, Rejected, Unflagged, NotRejected };

struct PhotoFilter {
    std::optional<int64_t> folder_id;
    bool include_subfolders = true;
    int min_rating = 0;
    FlagFilter flag = FlagFilter::Any;
    std::optional<int64_t> tag_id;  // 子孫のタグも含む
    std::string date_from;          // "YYYY-MM-DD"（含む）。空なら制限なし
    std::string date_to;            // "YYYY-MM-DD"（含む）
    bool include_unavailable = true;  // false ならファイルなし・非対応を除く
    std::optional<int64_t> album_id;  // このアルバムの写真だけ（v3.16）
    bool recent_import = false;       // 最後に写真を足した取り込み（scan_root）の写真だけ（v3.16）
};

struct ScanStats {
    int added = 0;
    int updated = 0;    // サイズか更新日時が変わって読み直した
    int unchanged = 0;
    int missing = 0;    // 今回のスキャンでファイルなしになった
    int restored = 0;   // ファイルなしから戻った
    int renamed = 0;    // 大文字小文字だけが違う名前に変わった（同じ写真として扱う）
    int unsupported = 0;
    int folders_added = 0;
    int thumbnails = 0;
    int thumbnail_failures = 0;
};

struct ScanOptions {
    const ThumbnailCache* thumbnails = nullptr;  // 指定すればサムネイルも作る（キャッシュ済みは飛ばす）
    unsigned threads = 0;                        // 0 ならコア数 - 1（4.2 章のサムネイルプール）
    std::function<void(int done, int total)> progress;
    const std::atomic<bool>* cancel = nullptr;
};

class Catalog {
public:
    static std::unique_ptr<Catalog> open(const std::filesystem::path& path);
    ~Catalog();

    const std::filesystem::path& path() const { return path_; }
    // 開いたときにマイグレーションのためのバックアップを作った場合、そのパス
    std::filesystem::path migration_backup() const;

    // ルートフォルダを登録する。登録済みならその id を返す
    int64_t add_root(const std::filesystem::path& dir, std::string_view label = {});
    std::vector<RootInfo> roots();
    std::optional<RootInfo> root_for_path(const std::filesystem::path& dir);

    // ルート以下を走査してカタログを更新する（取り込み・再スキャン共通）。
    // ルートのフォルダにアクセスできない場合（外付けドライブを外した等）は何も変えずに Error を投げる。
    ScanStats scan_root(int64_t root_id, const ScanOptions& options = {});

    std::vector<FolderInfo> folders(int64_t root_id);
    std::optional<int64_t> folder_id(int64_t root_id, std::string_view rel_path);

    int64_t count(const PhotoFilter& filter);
    std::vector<PhotoRecord> query(const PhotoFilter& filter, int64_t offset = 0, int64_t limit = -1);
    // 絞り込み結果の id だけを query と同じ順で返す（10 万件のグリッド用。行データは photos_by_ids で必要な分だけ読む）
    std::vector<int64_t> query_ids(const PhotoFilter& filter);
    // 指定した id の写真。返す順は ids と同じ（存在しない id は飛ばす）
    std::vector<PhotoRecord> photos_by_ids(std::span<const int64_t> ids);
    std::optional<PhotoRecord> photo(int64_t id);
    // ディスク上で実際に開けるパス。見つからなければ nullopt
    std::optional<std::filesystem::path> photo_disk_path(int64_t id);

    void set_rating(std::span<const int64_t> ids, int rating);
    void set_flag(std::span<const int64_t> ids, int flag);

    // "親/子/孫" 形式のパスでタグを作る（途中の階層も作る）。既存なら id を返す
    int64_t ensure_tag(std::string_view path);
    std::optional<int64_t> find_tag(std::string_view path);
    std::vector<TagInfo> tags();
    void add_tag(std::span<const int64_t> ids, int64_t tag_id);
    void remove_tag(std::span<const int64_t> ids, int64_t tag_id);
    std::vector<TagInfo> photo_tags(int64_t photo_id);

    // アルバム（v3.16）。写真は参照するだけで、アルバムを消しても写真は消えない。名前は NFC で、重複は Error
    std::vector<AlbumInfo> albums();
    int64_t create_album(std::string_view name);
    void rename_album(int64_t album_id, std::string_view name);
    void delete_album(int64_t album_id);
    void add_to_album(int64_t album_id, std::span<const int64_t> ids);
    void remove_from_album(int64_t album_id, std::span<const int64_t> ids);

    // 編集パラメータ（6.1 章の JSON、edits テーブル）。編集がなければ nullopt
    std::optional<std::string> edit_json(int64_t photo_id);
    // 書き込みスレッドに積む（完了を待たない）。json が nullopt なら行を消す（編集なし = すべて既定値、6.1 章）
    void save_edit(int64_t photo_id, int process_version, std::optional<std::string> json);
    // それまでに積んだ書き込みがすべて終わるまで待つ
    void flush();

private:
    explicit Catalog(const std::filesystem::path& path);

    std::filesystem::path path_;
    std::unique_ptr<db::DbWriter> writer_;
    std::unique_ptr<db::Database> reader_;
    std::mutex reader_mutex_;
};

} // namespace focal
