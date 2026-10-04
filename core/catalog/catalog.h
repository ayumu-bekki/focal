#pragma once

#include <atomic>
#include <ctime>
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
    std::string path;  // 絶対パス（NFC、区切りは '/'）。ボリュームがあるときは最後に見たマウントポイントからの位置
    std::string label;
    // v3.19: ボリュームの ID と、ボリュームのルートからの相対パス。ボリュームを判別できなければ空
    std::string volume_id;
    std::string volume_name;
    std::string volume_rel_path;
    bool online = true;  // ルートのフォルダにいまアクセスできる（外付けドライブが外れていると false）
};

// ルートの詳しい情報（サイドバーの「情報を見る」、v3.19）
struct RootDetails {
    RootInfo root;
    std::string mount_point;       // ボリュームのマウントポイント。オフラインなど、わからなければ空
    std::string fs_type;           // ファイルシステム。わからなければ空
    int kind = 0;                  // 0 不明、1 内蔵、2 外付け、3 ネットワーク（util/volume.h の VolumeKind と同じ並び）
    int64_t total_bytes = -1;      // ボリュームの全体の容量。取れなければ -1
    int64_t free_bytes = -1;       // ボリュームの空き容量。取れなければ -1
    int64_t photos = 0;            // 写真の枚数（ファイルなしを含む）
    int64_t folders = 0;           // サブフォルダの数（ルート自身は数えない）
    int64_t missing = 0;           // ファイルなしの枚数
    int64_t edited_photos = 0;     // 現像・★・フラグ・タグのいずれかが付いた写真の枚数（外すと消える情報の目安）
    int64_t total_file_bytes = 0;  // 写真のファイルの合計サイズ
    std::string capture_from;      // 撮影日の範囲（'YYYY-MM-DD'）。撮影日のある写真がなければ空
    std::string capture_to;
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

// アルバム（手で集めた写真）、アルバムのフォルダ（入れ子のための入れ物）、スマートアルバム（保存した検索条件、読み取り専用）
enum class AlbumKind { Album = 0, Folder = 1, Smart = 2 };

struct AlbumInfo {
    int64_t id = 0;
    std::string name;
    int64_t photo_count = 0;  // フォルダは 0。スマートアルバムは条件に合う枚数
    std::optional<int64_t> parent_id;  // 入れ子のとき、親のフォルダ（v3.19）
    AlbumKind kind = AlbumKind::Album;
    std::optional<int64_t> cover_photo_id;
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
    std::optional<int64_t> smart_album_id;  // このスマートアルバムの条件に合う写真だけ（v3.19）
    bool recent_import = false;       // 最後に写真を足した取り込み（scan_root）の写真だけ（v3.16）
};

struct ScanStats {
    int added = 0;
    int updated = 0;    // サイズか更新日時が変わって読み直した
    int unchanged = 0;
    int missing = 0;    // 今回のスキャンでファイルなしになった
    int restored = 0;   // ファイルなしから戻った
    int renamed = 0;    // 大文字小文字だけが違う名前に変わった（同じ写真として扱う）
    int inherited = 0;  // コピー（同じ内容のファイルが別の場所に残っている）として、元の現像・★・フラグ・タグを引き継いだ写真の数（v3.19）
    int relinked = 0;   // ほかのフォルダから移動してきた（ファイル名・サイズ・撮影日時が同じ）写真をつなぎ直した（v3.19）
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

    // ルートフォルダを登録する。登録済みならその id を返す。
    // v3.19: すでに登録したルートの**中**のフォルダなら、新しいルートは作らず、そのルートの id を返す（同じ写真を二重に登録しない）。
    // 登録済みのルートを**含む**フォルダなら、含まれるルートの写真・フォルダを新しいルートへ移して統合する
    // （現像・★・フラグ・タグ・アルバムの所属は、写真の行ごと引き継ぐ。同じファイルの行が両方にあれば、情報の多い方を残して合わせる）。
    // 登録済みのルートを含むときは、変更の前にカタログのバックアップを作る
    int64_t add_root(const std::filesystem::path& dir, std::string_view label = {});

    // 重なっているルート（別のルートの中にあるルート）。v3.19 より前のカタログに、二重登録の跡として残りうる
    struct NestedRoot {
        int64_t parent_id = 0;
        int64_t child_id = 0;
    };
    std::vector<NestedRoot> nested_roots();
    // 重なっているルートをすべて統合する（add_root の統合と同じ）。統合したルートの数を返す。変更の前にバックアップを作る
    int merge_nested_roots();

    // dir を含む登録済みのルートと、その中のフォルダ。ルートの外ならなし（スキャン前はフォルダの行がなく、なしになることがある）
    struct FolderLocation {
        int64_t root_id = 0;
        int64_t folder_id = 0;
    };
    std::optional<FolderLocation> folder_for_path(const std::filesystem::path& dir);

    // ルートの場所を付け替える（移動した・ドライブが変わったなど）。写真の行（現像・★・タグ）とルートの相対パスはそのまま。
    // 付け替えたあとにスキャンすると、新しい場所にあるものは正常に、ないものは「ファイルなし」になる。
    // 新しい場所が別のルートと重なる・同じなら Error
    void relocate_root(int64_t root_id, const std::filesystem::path& new_dir);

    // カタログのバックアップ（VACUUM INTO）。カタログのファイルの隣に "<名前>.before-<reason>-<日時>.bak" を作り、
    // 同じ reason の古いものは新しい 5 つだけ残す。作ったファイルを返す
    std::filesystem::path backup(std::string_view reason);
    std::vector<RootInfo> roots();
    std::optional<RootInfo> root_for_path(const std::filesystem::path& dir);
    // dir を含む（dir 自身も含む）登録済みのルート。いちばん深いもの
    std::optional<RootInfo> root_containing(const std::filesystem::path& dir);
    // マウントされているボリュームを見て、ルートの path をいまのマウントポイントに合わせる（v3.19）。
    // 変えたルートの数を返す。ボリュームが外れているルートは変えない（roots() で online = false になる）
    int refresh_volumes();
    // ルートをカタログから外す（v3.19）。そのルートの写真・フォルダの情報と、★・フラグ・タグ・アルバムへの所属・編集も消える。
    // ディスク上のファイルには触れない。サムネイルのキャッシュは残る
    // v3.19: 外す前に、自動でバックアップを作る（backup("remove-root")）
    void remove_root(int64_t root_id);
    // ルートの詳しい情報。ボリュームの容量は、応答しない共有で待ち続けないよう時間切れ（1.5 秒）で諦める（-1）
    RootDetails root_details(int64_t root_id);
    // ルートの表示名を変える（空なら消す）
    void set_root_label(int64_t root_id, std::string_view label);

    // ルート以下を走査してカタログを更新する（取り込み・再スキャン共通）。
    // ルートのフォルダにアクセスできない場合（外付けドライブを外した等）は何も変えずに Error を投げる。
    ScanStats scan_root(int64_t root_id, const ScanOptions& options = {});

    // 指定したファイルだけを登録する（v3.19。カードの取り込みで、コピーしたファイルを登録するのに使う）。
    // ルートを走査しない: ほかのファイルの追加・削除・サムネイルには触れず、時間は渡したファイルの数だけで決まる。
    // files はルートの下の RAW の絶対パス。ルートの外・RAW でないものは Error(InvalidArgument)。
    // 登録済みで変わっていないものは unchanged、サイズか更新日時が変わっていれば updated として読み直す。
    // 足りないフォルダの行（親も）は作る。写真を足したら「最近の取り込み」の時刻を記録する
    ScanStats register_files(int64_t root_id, std::span<const std::filesystem::path> files,
                             const ScanOptions& options = {});

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

    // アルバム（v3.16）。写真は参照するだけで、アルバムを消しても写真は消えない。名前は NFC で、同じ親の下で重複は Error。
    // v3.19: フォルダで入れ子にできる（親になれるのはフォルダだけ）。albums() は親が先、同じ親の中は作った順
    std::vector<AlbumInfo> albums();
    int64_t create_album(std::string_view name, std::optional<int64_t> parent_id = std::nullopt);
    int64_t create_album_folder(std::string_view name, std::optional<int64_t> parent_id = std::nullopt);
    void rename_album(int64_t album_id, std::string_view name);
    // フォルダを消すと中のアルバムも消える（写真は消えない）
    void delete_album(int64_t album_id);
    // 親を変える（nullopt でいちばん上へ）。自分の中へは動かせない
    void move_album(int64_t album_id, std::optional<int64_t> parent_id);
    // 手で集めるアルバムだけに足せる（フォルダ・スマートアルバムは Error）
    void add_to_album(int64_t album_id, std::span<const int64_t> ids);
    void remove_from_album(int64_t album_id, std::span<const int64_t> ids);
    // カバー写真。そのアルバムの写真でなければ Error。nullopt で自動（先頭の写真）に戻す。
    // albums() の cover_photo_id は、指定がなければ先頭の写真（手で集めるアルバムだけ。空なら nullopt）
    void set_album_cover(int64_t album_id, std::optional<int64_t> photo_id);
    // 同じ親の中で、サイドバーの並びを 1 つ上（delta < 0）・下（delta > 0）へ動かす。端なら何もしない（v3.19）
    void move_album_order(int64_t album_id, int delta);

    // スマートアルバム（v3.19）。条件は smart_query.h の JSON（不正なら Error）
    int64_t create_smart_album(std::string_view name, const std::string& query_json,
                               std::optional<int64_t> parent_id = std::nullopt);
    void set_smart_query(int64_t album_id, const std::string& query_json);
    std::optional<std::string> smart_query(int64_t album_id);

    // ファイル名・サイズ・撮影日時がすべて同じ写真（取り込み済みかの判定、v3.19）。なければ nullopt
    std::optional<int64_t> find_photo_by_identity(std::string_view file_name, int64_t file_size,
                                                  std::string_view capture_time);
    // フォルダ（ルートからの相対パス）とファイル名で写真を探す
    std::optional<int64_t> find_photo(int64_t root_id, std::string_view folder_rel_path, std::string_view file_name);

    // 写真の行をカタログから消す（★・フラグ・タグ・アルバムの所属・編集も消える）。ファイルには触れない。
    // ファイルも消すときは catalog/photo_delete.h の delete_photos を使う
    void remove_photos(std::span<const int64_t> ids);

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

// カタログに取り込む RAW のファイル名か（拡張子で判定）
bool is_raw_file_name(std::string_view name);
// 撮影日時の保存形式 'YYYY-MM-DDTHH:MM:SS'（ローカル時刻）。t <= 0 なら nullopt
std::optional<std::string> capture_time_string(std::time_t t);

} // namespace focal
