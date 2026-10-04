/*
 * focal C API（design.md 4.3 章、ADR-13）
 *
 * core（C++）と UI の唯一の境界。macOS では Swift ラッパー（FocalCore）から使う。
 *
 * 規約
 * - 名前はすべて fc_ 接頭辞。
 * - 戻り値が fc_status の関数は、失敗時に詳細を fc_last_error() で返す。
 * - 文字列の入力はすべて UTF-8 の NUL 終端。パスも UTF-8。
 * - fc_*_array は core が確保し、対応する fc_*_array_free で解放する。中の文字列も配列が持つ。
 * - 画像バッファは呼び出し側が確保する（M3 以降）。
 * - スレッド: 特に書いていない関数は、どのスレッドから呼んでもよい（同じハンドルを複数スレッドから使ってよい）。
 *   コールバックは core のワーカースレッドから呼ばれる。UI の更新は呼び出し側でメインスレッドに戻すこと。
 * - C++ 例外はこの境界の外に出さない。
 */
#ifndef FOCAL_FOCAL_H
#define FOCAL_FOCAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FC_API_VERSION 12

typedef enum fc_status {
    FC_OK = 0,
    FC_ERR_INVALID_ARGUMENT = 1,
    FC_ERR_IO = 2,
    FC_ERR_UNSUPPORTED = 3,
    FC_ERR_DECODE = 4,
    FC_ERR_INTERNAL = 5,
    FC_ERR_CANCELLED = 6,
    FC_ERR_DATABASE = 7,
    FC_ERR_NOT_FOUND = 8,
    FC_ERR_NOT_READY = 9, /* 写真のデコードがまだ終わっていない */
} fc_status;

/* ヘッダの FC_API_VERSION とライブラリが一致するか、ラッパーの初期化時に確認する */
int32_t fc_api_version(void);

/* このスレッドで最後に失敗した呼び出しのメッセージ。次の fc_ 呼び出しまで有効。失敗がなければ "" */
const char* fc_last_error(void);

/* ---- カタログ ---------------------------------------------------------- */

typedef struct fc_catalog fc_catalog;

/* カタログを開く（なければ作る。古いスキーマなら移行する） */
fc_status fc_catalog_open(const char* path, fc_catalog** out);
/* NULL でもよい。実行中のスキャンがあれば先に fc_task_release で手放しておくこと */
void fc_catalog_close(fc_catalog* catalog);
/* 書き込みスレッドに積んだ変更（編集の保存など）がすべて終わるまで待つ。アプリの終了時に呼ぶ */
fc_status fc_catalog_flush(fc_catalog* catalog);

typedef struct fc_root {
    int64_t id;
    const char* path;  /* NFC の絶対パス */
    const char* label; /* なければ "" */
    /* v3.19: ボリュームの ID・名前と、ボリュームのルートからの相対パス。判別できなければ "" */
    const char* volume_id;
    const char* volume_name;
    const char* volume_rel_path;
    int32_t online; /* 1: いまアクセスできる。0: 外付けドライブが外れているなど（オフライン） */
} fc_root;

typedef struct fc_root_array {
    size_t count;
    const fc_root* items;
} fc_root_array;

fc_status fc_catalog_add_root(fc_catalog* catalog, const char* dir, int64_t* out_root_id);
fc_status fc_catalog_roots(fc_catalog* catalog, fc_root_array** out);
void fc_root_array_free(fc_root_array* array);
/* マウントされているボリュームを見て、ルートの場所をいまのマウントポイントに合わせる（v3.19）。
   外付けドライブをつなぎ直したとき・アプリの起動時に呼ぶ。変えたルートの数を out_changed に返す（NULL 可） */
fc_status fc_catalog_refresh_volumes(fc_catalog* catalog, int32_t* out_changed);
/* ルートをカタログから外す。写真の情報（★・フラグ・タグ・アルバムへの所属・編集）も消える。ディスク上のファイルは消さない */
fc_status fc_catalog_remove_root(fc_catalog* catalog, int64_t root_id);
/* ルートの表示名（空文字で消す） */
fc_status fc_catalog_set_root_label(fc_catalog* catalog, int64_t root_id, const char* label);

typedef struct fc_folder {
    int64_t id;
    int64_t root_id;
    int64_t parent_id; /* ルート自身は 0 */
    const char* rel_path; /* ルートからの相対パス（ルート自身は ""） */
    int64_t photo_count;  /* このフォルダ直下の写真の数 */
} fc_folder;

typedef struct fc_folder_array {
    size_t count;
    const fc_folder* items;
} fc_folder_array;

fc_status fc_catalog_folders(fc_catalog* catalog, int64_t root_id, fc_folder_array** out);
void fc_folder_array_free(fc_folder_array* array);

typedef struct fc_tag {
    int64_t id;
    int64_t parent_id; /* ルート直下は 0 */
    const char* name;
    const char* path; /* "親/子" */
    int64_t photo_count;
} fc_tag;

typedef struct fc_tag_array {
    size_t count;
    const fc_tag* items;
} fc_tag_array;

fc_status fc_catalog_tags(fc_catalog* catalog, fc_tag_array** out);
fc_status fc_catalog_photo_tags(fc_catalog* catalog, int64_t photo_id, fc_tag_array** out);
void fc_tag_array_free(fc_tag_array* array);

/* "親/子" 形式。途中の階層も作る。既存なら その id */
fc_status fc_catalog_ensure_tag(fc_catalog* catalog, const char* path, int64_t* out_tag_id);
fc_status fc_catalog_add_tag(fc_catalog* catalog, const int64_t* photo_ids, size_t count, int64_t tag_id);
fc_status fc_catalog_remove_tag(fc_catalog* catalog, const int64_t* photo_ids, size_t count, int64_t tag_id);

/* アルバム（v3.16）: 利用者が選んだ写真の集まり。写真は参照するだけで、アルバムを消しても写真は消えない。
   v3.19: フォルダで入れ子にできる。スマートアルバムは保存した検索条件（読み取り専用） */
typedef enum fc_album_kind {
    FC_ALBUM_ALBUM = 0,  /* 手で集める */
    FC_ALBUM_FOLDER = 1, /* アルバムを入れる入れ物（写真は持たない） */
    FC_ALBUM_SMART = 2,  /* 条件に合う写真。写真を足せない */
} fc_album_kind;

typedef struct fc_album {
    int64_t id;
    const char* name;
    int64_t photo_count; /* フォルダは 0。スマートアルバムは条件に合う枚数 */
    int64_t parent_id;   /* いちばん上は 0 */
    int32_t kind;        /* fc_album_kind */
    int64_t cover_photo_id; /* 指定がなければ 0（先頭の写真を使う） */
} fc_album;

typedef struct fc_album_array {
    size_t count;
    const fc_album* items;
} fc_album_array;

/* 親が先、同じ親の中は作った順（深さ優先） */
fc_status fc_catalog_albums(fc_catalog* catalog, fc_album_array** out);
void fc_album_array_free(fc_album_array* array);
/* 名前は前後の空白を落とす。空や、同じ親の下に同じ名前があれば FC_ERR_INVALID_ARGUMENT。
   parent_id: 0 ならいちばん上。親にできるのはフォルダだけ */
fc_status fc_catalog_create_album(fc_catalog* catalog, const char* name, int64_t parent_id, int64_t* out_album_id);
fc_status fc_catalog_create_album_folder(fc_catalog* catalog, const char* name, int64_t parent_id,
                                         int64_t* out_album_id);
/* query_json は design.md 7.2 章（catalog/smart_query.h）の条件。不正なら FC_ERR_INVALID_ARGUMENT */
fc_status fc_catalog_create_smart_album(fc_catalog* catalog, const char* name, const char* query_json,
                                        int64_t parent_id, int64_t* out_album_id);
fc_status fc_catalog_set_smart_query(fc_catalog* catalog, int64_t album_id, const char* query_json);
fc_status fc_catalog_rename_album(fc_catalog* catalog, int64_t album_id, const char* name);
/* フォルダを消すと中のアルバムも消える（写真は消えない） */
fc_status fc_catalog_delete_album(fc_catalog* catalog, int64_t album_id);
/* 親を変える（0 でいちばん上）。自分の中へは動かせない */
fc_status fc_catalog_move_album(fc_catalog* catalog, int64_t album_id, int64_t parent_id);
/* 写真を足せるのは FC_ALBUM_ALBUM だけ */
fc_status fc_catalog_add_to_album(fc_catalog* catalog, int64_t album_id, const int64_t* photo_ids, size_t count);
fc_status fc_catalog_remove_from_album(fc_catalog* catalog, int64_t album_id, const int64_t* photo_ids, size_t count);
/* 同じ親の中でサイドバーの並びを動かす。delta < 0 で上、> 0 で下。端なら何もしない */
fc_status fc_catalog_move_album_order(fc_catalog* catalog, int64_t album_id, int32_t delta);
/* photo_id が 0 ならカバーの指定をやめる（先頭の写真になる）。そのアルバムの写真でなければ FC_ERR_INVALID_ARGUMENT。
   fc_album.cover_photo_id は、指定がなければ先頭の写真 */
fc_status fc_catalog_set_album_cover(fc_catalog* catalog, int64_t album_id, int64_t photo_id);

/* core が持つ文字列（fc_string_free で解放する） */
typedef struct fc_string {
    const char* value;
} fc_string;
void fc_string_free(fc_string* string);
/* スマートアルバムの条件 JSON。スマートアルバムでなければ FC_ERR_NOT_FOUND */
fc_status fc_catalog_smart_query(fc_catalog* catalog, int64_t album_id, fc_string** out);

/* ---- 写真 -------------------------------------------------------------- */

typedef enum fc_photo_status {
    FC_PHOTO_OK = 0,
    FC_PHOTO_MISSING = 1,
    FC_PHOTO_UNSUPPORTED = 2,
} fc_photo_status;

typedef struct fc_photo {
    int64_t id;
    int64_t folder_id;
    const char* file_name;
    const char* path;         /* NFC の絶対パス */
    int32_t status;           /* fc_photo_status */
    const char* capture_time; /* "YYYY-MM-DDTHH:MM:SS"（ローカル時刻）。不明なら NULL */
    const char* camera_make;  /* 不明なら "" */
    const char* camera_model;
    const char* lens_model;
    int64_t iso;           /* 不明なら 0 */
    double exposure_time;  /* 秒。不明なら 0 */
    double f_number;       /* 不明なら 0 */
    double focal_length;   /* mm。不明なら 0 */
    int32_t width;         /* 向き補正後 */
    int32_t height;
    int32_t orientation;   /* LibRaw の flip 値 */
    int32_t rating;        /* 0..5 */
    int32_t flag;          /* -1: リジェクト、0: なし、1: ピック */
    int64_t file_size;
    int64_t file_mtime;
} fc_photo;

typedef struct fc_photo_array {
    size_t count;
    const fc_photo* items;
} fc_photo_array;

typedef struct fc_id_array {
    size_t count;
    const int64_t* items;
} fc_id_array;

typedef enum fc_flag_filter {
    FC_FLAG_ANY = 0,
    FC_FLAG_PICKED = 1,
    FC_FLAG_REJECTED = 2,
    FC_FLAG_UNFLAGGED = 3,
    FC_FLAG_NOT_REJECTED = 4,
} fc_flag_filter;

typedef struct fc_photo_filter {
    int64_t folder_id;          /* 0 なら全フォルダ */
    int32_t include_subfolders; /* 0 / 1 */
    int32_t min_rating;         /* 0..5 */
    int32_t flag;               /* fc_flag_filter */
    int64_t tag_id;             /* 0 なら制限なし。子孫のタグも含む */
    const char* date_from;      /* "YYYY-MM-DD" か NULL */
    const char* date_to;        /* "YYYY-MM-DD" か NULL（その日を含む） */
    int32_t include_unavailable; /* 0 ならファイルなし・非対応を除く */
    int64_t album_id;           /* 0 なら制限なし。このアルバムの写真だけ（v3.16） */
    int32_t recent_import;      /* 1 なら最後に写真を足した取り込みの写真だけ（v3.16） */
    int64_t smart_album_id;     /* 0 なら制限なし。このスマートアルバムの条件に合う写真だけ（v3.19） */
} fc_photo_filter;

/* 既定値（制限なし、サブフォルダを含む、ファイルなしも含む）で初期化する */
void fc_photo_filter_init(fc_photo_filter* filter);

fc_status fc_catalog_count(fc_catalog* catalog, const fc_photo_filter* filter, int64_t* out);
/* 撮影日時順（不明は最後）。limit < 0 なら全件 */
fc_status fc_catalog_query(fc_catalog* catalog, const fc_photo_filter* filter, int64_t offset, int64_t limit,
                           fc_photo_array** out);
/* query と同じ順の id だけ（大量の写真のグリッド用） */
fc_status fc_catalog_query_ids(fc_catalog* catalog, const fc_photo_filter* filter, fc_id_array** out);
/* 指定した id の写真を ids の順で返す（存在しない id は飛ばす） */
fc_status fc_catalog_photos_by_ids(fc_catalog* catalog, const int64_t* ids, size_t count, fc_photo_array** out);
void fc_photo_array_free(fc_photo_array* array);
void fc_id_array_free(fc_id_array* array);

fc_status fc_catalog_set_rating(fc_catalog* catalog, const int64_t* photo_ids, size_t count, int32_t rating);
fc_status fc_catalog_set_flag(fc_catalog* catalog, const int64_t* photo_ids, size_t count, int32_t flag);

/* ---- スキャン（取り込み・再スキャン） ---------------------------------- */

typedef struct fc_scan_stats {
    int32_t added;
    int32_t updated;
    int32_t unchanged;
    int32_t missing;
    int32_t restored;
    int32_t renamed;
    int32_t unsupported;
    int32_t relinked; /* 別のフォルダから移動してきた写真をつなぎ直した（v3.19） */
    int32_t folders_added;
    int32_t thumbnails;
    int32_t thumbnail_failures;
} fc_scan_stats;

typedef struct fc_task fc_task;

/* 進捗。ワーカースレッドから呼ばれる */
typedef void (*fc_scan_progress_fn)(void* user, int32_t done, int32_t total);
/* 完了。必ず 1 回だけ呼ばれる（キャンセル時は FC_ERR_CANCELLED）。stats と message はコールバックの間だけ有効 */
typedef void (*fc_scan_done_fn)(void* user, fc_status status, const fc_scan_stats* stats, const char* message);

/* root_id 以下を core のスレッドで走査する。thumbnail_cache_dir が NULL でなければサムネイルも作る。
   catalog は完了まで閉じないこと。out_task は fc_task_release で手放す */
fc_status fc_catalog_scan_async(fc_catalog* catalog, int64_t root_id, const char* thumbnail_cache_dir,
                                fc_scan_progress_fn progress, fc_scan_done_fn done, void* user, fc_task** out_task);
void fc_task_cancel(fc_task* task);
/* 実行中なら完了まで待ってから解放する */
void fc_task_release(fc_task* task);

/* ---- 写真の削除（v3.19、design.md 5.11 章） ------------------------------ */

/* 削除の前に、何がどうなるかを数える。写真 = カタログの RAW と、同じフォルダで同じ名前の幹の JPEG・動画・サイドカー */
typedef struct fc_delete_plan {
    int32_t photos;
    int32_t files;          /* 消すファイルの数（すでにないものは数えない） */
    int32_t network_photos; /* ネットワークボリューム（ゴミ箱を使わず完全に消える）にある写真の数 */
    int32_t missing_photos; /* ファイルがすでにない写真（カタログの情報だけ消える） */
} fc_delete_plan;

fc_status fc_catalog_plan_delete(fc_catalog* catalog, const int64_t* photo_ids, size_t count, fc_delete_plan* out);

/* ローカルのファイルをゴミ箱へ送る（OS の操作なので呼び出し側が行う）。成功なら 0 を返す。
   ネットワークボリュームのファイルには呼ばれず、core が即座に完全削除する */
typedef int32_t (*fc_trash_fn)(void* user, const char* path);

typedef struct fc_delete_result {
    int32_t photos_deleted; /* ファイルを消し、カタログからも消した写真 */
    int32_t photos_failed;  /* RAW を消せなかったので、ファイルもカタログも残した写真 */
    int32_t files_trashed;
    int32_t files_removed;  /* 完全に削除したファイル */
    int32_t files_failed;   /* 同時に消すファイルの失敗も含む */
} fc_delete_result;

/* 写真を削除する。RAW を消せた写真だけカタログから消す（★・フラグ・タグ・アルバムの所属・編集も消える）。
   trash が NULL なら、ローカルのファイルは消さず失敗として扱う。errors は失敗の内容（改行区切り）で、
   NULL でもよい。fc_string_free で解放する。時間がかかることがあるので、メインスレッドで呼ばないこと */
fc_status fc_catalog_delete_photos(fc_catalog* catalog, const int64_t* photo_ids, size_t count, fc_trash_fn trash,
                                   void* user, fc_delete_result* out, fc_string** errors);

/* ---- カードの取り込み（v3.19、design.md 5.10 章） ----------------------- */

/* DCIM フォルダを持つボリューム（SD カードなど） */
typedef struct fc_import_source {
    const char* volume_id;   /* 取れなければ "" */
    const char* name;
    const char* mount_point;
    const char* dcim_path;
    int32_t removable;
} fc_import_source;

typedef struct fc_import_source_array {
    size_t count;
    const fc_import_source* items;
} fc_import_source_array;

fc_status fc_import_sources(fc_import_source_array** out);
void fc_import_source_array_free(fc_import_source_array* array);

typedef struct fc_card_summary {
    int32_t shots;  /* RAW + JPEG のペアなどは 1 枚 */
    int32_t files;
    int64_t bytes;
} fc_card_summary;

/* カードの中身の概算（ファイルの一覧と大きさだけ読む）。遅いカードでは時間がかかるのでメインスレッドで呼ばないこと。
   source は DCIM フォルダ、または DCIM を持つボリューム */
fc_status fc_card_summarize(const char* source, fc_card_summary* out);

typedef struct fc_card_import_options {
    const char* source;
    const char* dest_root;           /* コピー先。<dest_root>/YYYY/YYYY-MM-DD/ に入れる。登録されたルートの下でなければ登録する */
    int32_t verify;                  /* 1: コピーを読み直してハッシュで照合する */
    int32_t dry_run;                 /* 1: コピーも登録もせず数えるだけ */
    int64_t album_id;                /* 0 なら足さない。手で集めるアルバム */
    const int64_t* tag_ids;          /* 取り込んだ写真に付けるタグ */
    size_t tag_count;
    const char* thumbnail_cache_dir; /* NULL でなければ登録のときにサムネイルも作る */
} fc_card_import_options;

typedef enum fc_import_phase {
    FC_IMPORT_READING = 0,    /* カードを読んで、撮影日時と取り込み済みかを調べる */
    FC_IMPORT_COPYING = 1,
    FC_IMPORT_CATALOGING = 2, /* カタログへの登録 */
} fc_import_phase;

typedef struct fc_card_import_result {
    int32_t shots;
    int32_t imported;
    int32_t skipped_duplicates;
    int32_t failed;
    int32_t estimated_dates; /* 撮影日時が読めず、ファイルの更新日時で日付フォルダを決めた枚数 */
    int32_t files_copied;
    int64_t bytes_copied;
    int32_t cancelled;
    int64_t root_id;         /* 登録先のルート。dry_run では 0 */
    int32_t added;           /* カタログに新しく足した写真の数 */
    int64_t bytes_needed;    /* コピーするはずの大きさ（取り込み済みを除く） */
    int64_t space_available; /* 読み込み先の空き容量。取れなければ -1 */
} fc_card_import_result;

/* path（なければいちばん近い親）があるボリュームの空き容量。取れなければ -1 */
int64_t fc_free_space(const char* path);

/* 進捗。ワーカースレッドから呼ばれる。current はコールバックの間だけ有効 */
typedef void (*fc_card_progress_fn)(void* user, int32_t phase, int32_t done, int32_t total, int64_t bytes_done,
                                    int64_t bytes_total, const char* current);
/* 完了。必ず 1 回だけ呼ばれる。status が FC_OK でも一部の写真が失敗していることがある（result->failed。
   message に失敗の内容を改行区切りで入れる）。result と message はコールバックの間だけ有効 */
typedef void (*fc_card_done_fn)(void* user, fc_status status, const fc_card_import_result* result,
                                const char* message);

/* カードの写真を core のスレッドでコピーして登録する。カードには書き込まない。
   キャンセルしても、それまでにコピーした分は登録する。catalog は完了まで閉じないこと。out_task は fc_task_release で手放す */
fc_status fc_card_import_start(fc_catalog* catalog, const fc_card_import_options* options,
                               fc_card_progress_fn progress, fc_card_done_fn done, void* user, fc_task** out_task);

/* ---- 書き出し（5.8 章） ------------------------------------------------- */

typedef enum fc_export_format {
    FC_EXPORT_JPEG = 0,   /* sRGB、8-bit、ICC 埋め込み */
    FC_EXPORT_TIFF16 = 1, /* sRGB、16-bit、ICC 埋め込み */
} fc_export_format;

typedef struct fc_export_options {
    int32_t format;   /* fc_export_format */
    int32_t quality;  /* JPEG の品質 1..100 */
    int32_t long_edge; /* 0 なら原寸 */
    const char* dest_dir;
} fc_export_options;

/* 1 枚ごとに呼ばれる。ok なら text は書き出したファイルのパス、失敗ならエラーメッセージ（コールバックの間だけ有効） */
typedef void (*fc_export_progress_fn)(void* user, int32_t done, int32_t total, int64_t photo_id, int32_t ok,
                                      const char* text);
/* 必ず 1 回だけ呼ばれる。status は FC_OK（全部処理した）か FC_ERR_CANCELLED */
typedef void (*fc_export_done_fn)(void* user, fc_status status);

/* 写真を順に書き出す（core のスレッド）。保存済みの編集を反映する。名前は {元のファイル名}.jpg / .tif で、
   あれば _1, _2 … を付ける（上書きしない）。catalog は完了まで閉じないこと。out_task は fc_task_release で手放す */
fc_status fc_export_start(fc_catalog* catalog, const int64_t* photo_ids, size_t count, const fc_export_options* options,
                          fc_export_progress_fn progress, fc_export_done_fn done, void* user, fc_task** out_task);

/* ---- サムネイル -------------------------------------------------------- */

typedef struct fc_thumbnailer fc_thumbnailer;

/* 結果。必ず 1 回だけ呼ばれる。path（キャッシュの JPEG、sRGB）はコールバックの間だけ有効。失敗・キャンセル時は NULL */
typedef void (*fc_thumbnail_fn)(void* user, uint64_t request_id, fc_status status, const char* path);

/* catalog は thumbnailer より長く生きていること。threads が 0 ならコア数 - 1 */
fc_status fc_thumbnailer_create(fc_catalog* catalog, const char* cache_dir, int32_t threads, fc_thumbnailer** out);
/* 開始前の要求はキャンセルとしてコールバックし、実行中の要求の完了を待つ */
void fc_thumbnailer_destroy(fc_thumbnailer* thumbnailer);
/* 後から来た要求を先に処理する。戻り値は要求 id（0 は失敗） */
uint64_t fc_thumbnailer_request(fc_thumbnailer* thumbnailer, int64_t photo_id, fc_thumbnail_fn callback, void* user);
/* 開始前ならキャンセルとしてコールバックする。実行中・完了済みなら何もしない */
void fc_thumbnailer_cancel(fc_thumbnailer* thumbnailer, uint64_t request_id);

/* ---- 現像（5 章・6 章・9.3 章） ---------------------------------------- */

typedef enum fc_wb_mode {
    FC_WB_AS_SHOT = 0,
    FC_WB_CUSTOM = 1,
} fc_wb_mode;

typedef enum fc_aspect {
    FC_ASPECT_FREE = 0,
    FC_ASPECT_ORIGINAL = 1,
    FC_ASPECT_1_1 = 2,
    FC_ASPECT_3_2 = 3,
    FC_ASPECT_4_3 = 4,
    FC_ASPECT_16_9 = 5,
    FC_ASPECT_5_4 = 6,
} fc_aspect;

/* 編集パラメータ（6.1 章）。JSON にない未知のキーは core 側のセッションが保持する */
typedef struct fc_settings {
    int32_t process_version;
    int32_t wb_mode;      /* fc_wb_mode */
    double temperature;   /* K（custom のとき） */
    double tint;
    double exposure;      /* EV */
    double contrast;      /* -100..100 */
    double highlights;
    double shadows;
    double whites;        /* -100..100 */
    double blacks;
    double brightness;
    double saturation;
    double vibrance;
    double clarity;       /* -200..200 */
    double sharpness;     /* 0..150 */
    double noise_reduction;        /* 0..100 輝度 */
    double color_noise_reduction;  /* 0..100 カラー */
    int32_t rotate90;     /* 0..3 */
    double straighten;    /* 度 */
    double crop_x, crop_y, crop_w, crop_h;
    int32_t aspect;       /* fc_aspect */
} fc_settings;

/* 既定値で初期化する */
void fc_settings_init(fc_settings* settings);

typedef struct fc_editor fc_editor;
typedef struct fc_session fc_session;

typedef enum fc_session_stage {
    FC_STAGE_OPENING = 0,
    FC_STAGE_PREVIEW = 1, /* 埋め込みプレビューを表示できる */
    FC_STAGE_READY = 2,   /* デコード済み。現像できる */
    FC_STAGE_FAILED = 3,
} fc_session_stage;

typedef enum fc_session_event {
    FC_EVENT_PREVIEW = 1,
    FC_EVENT_READY = 2,
    FC_EVENT_FAILED = 3,
} fc_session_event;

/* ワーカースレッドから呼ばれる。fc_session_close の後は呼ばれない。このコールバックの中で fc_session_close を呼ばないこと */
typedef void (*fc_session_fn)(void* user, int32_t event, const char* message);

typedef struct fc_session_info {
    int32_t stage;            /* fc_session_stage */
    int32_t oriented_width;   /* 向き補正後（ジオメトリ前） */
    int32_t oriented_height;
    int32_t output_width;     /* ジオメトリ適用後のフル解像度 */
    int32_t output_height;
    int32_t canvas_width;     /* 回転・傾き補正後、クロップ前（クロップ枠の座標の基準） */
    int32_t canvas_height;
    double as_shot_temperature; /* READY 以降。それまでは 0 */
    double as_shot_tint;
    int32_t preview_width;    /* 埋め込みプレビュー（sRGB、RGB 8-bit）。なければ 0 */
    int32_t preview_height;
    /* 1: プレビューは前に表示したときの現像結果（Display P3、ジオメトリ適用済み）。
       0: カメラの埋め込みプレビュー（sRGB、ジオメトリ前） */
    int32_t preview_display_p3;
} fc_session_info;

/* catalog は editor より長く生きていること。デコード・プレビュー・レンダリング・保存のスレッドを作る */
fc_status fc_editor_create(fc_catalog* catalog, fc_editor** out);
/* 未保存の編集を書き込む。開いているセッションはすべて先に閉じておくこと */
void fc_editor_destroy(fc_editor* editor);

/* 写真を開く（5.3 章）。すぐ戻り、PREVIEW → READY の順にコールバックする（先読み済みなら READY だけ）。
   preview_long_edge: 埋め込みプレビューの長辺、proxy_long_edge: ビューの長辺 × backingScaleFactor（上限 3840） */
fc_status fc_editor_open(fc_editor* editor, int64_t photo_id, int32_t preview_long_edge, int32_t proxy_long_edge,
                         fc_session_fn callback, void* user, fc_session** out);
/* 次の写真を 1 枚だけ先読みする */
void fc_editor_prefetch(fc_editor* editor, int64_t photo_id);

/* 現像結果のサムネイル（10 章）: 編集を保存するときと写真を閉じるとき、デコード済みのデータから作ってキャッシュに入れ、
   作り終えたら callback(user, photo_id) を保存スレッドから呼ぶ（編集を既定値に戻したときも呼ぶ） */
typedef void (*fc_thumbnail_updated_fn)(void* user, int64_t photo_id);
fc_status fc_editor_set_thumbnail_cache(fc_editor* editor, const char* cache_dir, fc_thumbnail_updated_fn callback,
                                        void* user);

/* 表示（フィット・100%）を描く GPU の名前（v3.14）。CPU で描くなら ""。editor と同じだけ有効 */
const char* fc_editor_gpu_name(fc_editor* editor);

/* 表示用の大きいプレビューのキャッシュ（写真を閉じるとき現像結果を入れ、次に開いたとき埋め込みプレビューより先に出す）。
   上限（バイト）を超えたら、最後に使った日時が古いものから消す */
fc_status fc_editor_set_preview_cache(fc_editor* editor, const char* cache_dir, uint64_t limit_bytes);
fc_status fc_editor_set_preview_cache_limit(fc_editor* editor, uint64_t limit_bytes);
/* 使用量（バイト）。キャッシュを設定していなければ 0 */
fc_status fc_editor_preview_cache_usage(fc_editor* editor, uint64_t* out_bytes);
fc_status fc_editor_clear_preview_cache(fc_editor* editor);

/* 編集をすぐ保存し、ハンドルを解放する（実行中のコールバックがあれば終わるまで待つ） */
void fc_session_close(fc_session* session);
fc_status fc_session_get_info(fc_session* session, fc_session_info* out);
/* プレビューを呼び出し側のバッファにコピーする（RGB 8-bit。色空間は fc_session_info の preview_display_p3） */
fc_status fc_session_copy_preview(fc_session* session, uint8_t* dst, size_t stride, size_t capacity);

fc_status fc_session_get_settings(fc_session* session, fc_settings* out);
/* 編集をすぐ保存し、書き込みが終わるまで待つ（書き出しの直前など） */
fc_status fc_session_save(fc_session* session);
/* begin_change と end_change の間なら途中経過、そうでなければそれ自体が 1 回の Undo になる。
   変更は 500ms 後に保存される（7.4 章） */
fc_status fc_session_set_settings(fc_session* session, const fc_settings* settings);
/* スライダーのドラッグなど、まとめて 1 回の Undo にする変更の開始と終了（9.3 章） */
void fc_session_begin_change(fc_session* session);
void fc_session_end_change(fc_session* session);
/* 戻した（やり直した）ら 1 */
int32_t fc_session_undo(fc_session* session);
int32_t fc_session_redo(fc_session* session);
int32_t fc_session_can_undo(fc_session* session);
int32_t fc_session_can_redo(fc_session* session);
/* ウィンドウの大きさが変わったとき（300ms のデバウンス後）に呼ぶ。次の FIT レンダリングでプロキシを作り直す */
void fc_session_set_proxy_long_edge(fc_session* session, int32_t long_edge);

typedef enum fc_render_mode {
    FC_RENDER_FIT = 0,    /* 出力画像全体を max_width × max_height に収める（プロキシから） */
    FC_RENDER_REGION = 1, /* 100% 表示: 出力画像の一部を 1 画素 = 1 画素で（フル解像度から） */
} fc_render_mode;

typedef enum fc_pixel_format {
    FC_PIXEL_RGB8 = 0,  /* R, G, B */
    FC_PIXEL_BGRX8 = 1, /* B, G, R, 255（Core Animation がそのまま使える 32-bit 形式） */
} fc_pixel_format;

typedef struct fc_render_request {
    int32_t mode; /* fc_render_mode */
    int32_t pixel_format; /* fc_pixel_format */
    int32_t ignore_crop;  /* 1: クロップモード。クロップを適用せずキャンバス全体を描く（座標もキャンバス） */
    int32_t max_width;
    int32_t max_height;
    double region_x; /* REGION: 出力画像の座標での左上 */
    double region_y;
    int32_t region_width;
    int32_t region_height;
    uint8_t* buffer; /* 呼び出し側が確保（Display P3）。コールバックが来るまで解放しないこと */
    size_t stride;
    size_t capacity;
} fc_render_request;

typedef struct fc_render_result {
    int32_t width; /* 書き込んだ大きさ */
    int32_t height;
    double scale;    /* 出力画像（フル解像度）に対する縮尺 */
    double region_x; /* REGION: 実際に描いた範囲の左上（端で切り詰めた後） */
    double region_y;
    uint32_t histogram[3][256]; /* 表示用出力の R/G/B */
} fc_render_result;

/* status: FC_OK / FC_ERR_CANCELLED（新しい要求に追い越された）/ FC_ERR_NOT_READY / その他の失敗。
   レンダースレッドから必ず 1 回呼ばれる。result は FC_OK のときだけ有効で、コールバックの間だけ使える */
typedef void (*fc_render_fn)(void* user, fc_status status, const fc_render_result* result);

/* latest-wins（4.2 章）: 新しい要求が来たら古い要求は打ち切られる（エディタ全体で 1 本）。戻り値は世代番号（0 は失敗） */
uint64_t fc_session_render(fc_session* session, const fc_render_request* request, fc_render_fn callback, void* user);

/* ---- クロップモードの操作（5.6 章） ------------------------------------
   クロップ枠はキャンバス（回転・傾き補正後）上の正規化矩形。キャンバスの大きさは fc_session_info の canvas_* */

typedef enum fc_crop_handle {
    FC_HANDLE_MOVE = 0,
    FC_HANDLE_LEFT, FC_HANDLE_RIGHT, FC_HANDLE_TOP, FC_HANDLE_BOTTOM,
    FC_HANDLE_TOP_LEFT, FC_HANDLE_TOP_RIGHT, FC_HANDLE_BOTTOM_LEFT, FC_HANDLE_BOTTOM_RIGHT,
} fc_crop_handle;

/* ハンドルのドラッグ。start はドラッグ開始時の設定、dx / dy は正規化座標での移動量。
   縦横比（settings->aspect）を保ち、画像からはみ出す分は縮めた枠を out に返す */
fc_status fc_crop_drag(const fc_settings* start, int32_t canvas_width, int32_t canvas_height, int32_t handle,
                       double dx, double dy, fc_settings* out);
/* 縦横比と傾き補正に合わせて、今の枠の中心を保った最大の枠にする（傾き補正・縦横比の変更時） */
fc_status fc_crop_fit(fc_settings* settings, int32_t canvas_width, int32_t canvas_height);
/* 時計回りに 90° × steps 回す（クロップ枠も一緒に回す） */
fc_status fc_crop_rotate(fc_settings* settings, int32_t steps);
/* 水平線ツール: キャンバス上の 2 点（画素）を結ぶ線が水平（縦に近ければ垂直）になる傾き補正値を返す */
double fc_crop_straighten_from_line(double x0, double y0, double x1, double y1, double current);

#ifdef __cplusplus
}
#endif

#endif /* FOCAL_FOCAL_H */
