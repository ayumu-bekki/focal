#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "util/volume.h"

namespace focal {

class ThumbnailCache;

// SD カードなどからの取り込み（v3.19、design.md 5.10 章・ADR-12）。
// カードのファイルをライブラリのルートの下 <ルート>/YYYY/YYYY-MM-DD/ にコピーし、カタログに登録する。
// カードには一切書き込まない（移動・削除はしない）。コピー先には新しいファイルを作るだけで、既存のファイルは上書きしない。

// RAW と一緒に扱うファイル（JPEG などの画像・動画・サイドカー）の名前か。RAW 自身は含まない
bool is_companion_file_name(const std::string& name);

// path（なければいちばん近い親）があるボリュームの空き容量（バイト）。取れなければ -1
int64_t free_space_bytes(const std::filesystem::path& path);

// DCIM フォルダを持つボリューム（DCF 規格のカメラのメディア）
struct ImportSource {
    VolumeInfo volume;
    std::filesystem::path dcim;  // ボリューム内の DCIM フォルダ
};
std::vector<ImportSource> detect_import_sources();

// カードの中身の概算（ファイルの一覧と大きさだけ。メタデータは読まないので速い）
struct CardSummary {
    int shots = 0;      // 1 枚と数える単位（RAW + JPEG のペアや、サイドカーは 1 枚）
    int files = 0;
    int64_t bytes = 0;
};
// source: DCIM フォルダ、または DCIM を持つボリューム
CardSummary summarize_card(const std::filesystem::path& source);

struct CardImportProgress {
    enum class Phase { Reading, Copying, Cataloging };
    Phase phase = Phase::Reading;
    int done = 0;  // Reading / Copying: 処理した枚数、Cataloging: 登録したファイル数
    int total = 0;
    int64_t bytes_done = 0;
    int64_t bytes_total = 0;
    std::string current;  // コピー中のファイル名
};

struct CardImportOptions {
    std::filesystem::path source;     // DCIM フォルダ、または DCIM を持つボリューム
    std::filesystem::path dest_root;  // コピー先（ライブラリのルート。登録されていなければ登録する）
    bool verify = true;               // コピー後に、書いたファイルを読み直してハッシュ（BLAKE3）を照合する
    bool dry_run = false;             // コピーも登録もせず、何がどうなるかだけ数える
    std::optional<int64_t> album_id;  // 取り込んだ写真を足す手で集めるアルバム
    std::vector<int64_t> tag_ids;     // 取り込んだ写真に付けるタグ
    std::optional<Settings> preset;   // 取り込んだ写真に重ねる現像のプリセット（調整だけ。なければ何もしない、v3.20）
    const ThumbnailCache* thumbnails = nullptr;  // 登録のときにサムネイルも作る
    const std::atomic<bool>* cancel = nullptr;
    std::function<void(const CardImportProgress&)> progress;
};

struct CardImportResult {
    int shots = 0;               // カードにあった枚数
    int imported = 0;            // コピーした枚数（dry_run では、コピーするはずの枚数）
    int skipped_duplicates = 0;  // 取り込み済みなのでコピーしなかった枚数
    int failed = 0;
    int estimated_dates = 0;     // 撮影日時を読めず、ファイルの更新日時で日付フォルダを決めた枚数
    int files_copied = 0;
    int64_t bytes_copied = 0;
    bool cancelled = false;
    int64_t bytes_needed = 0;      // コピーするはずの大きさ（取り込み済みを除く）
    int64_t space_available = -1;  // 読み込み先の空き容量。取れなければ -1
    std::vector<std::string> errors;
    std::optional<int64_t> root_id;      // 登録先のルート（dry_run では nullopt）
    ScanStats scan;                      // コピー後のカタログ登録
    std::vector<int64_t> photo_ids;      // 取り込んだ写真（カタログの id）
};

// 取り込みを実行する。失敗した 1 枚は errors に記録して続ける。キャンセルしても、それまでにコピーした分は登録する
CardImportResult import_from_card(Catalog& catalog, const CardImportOptions& options);

// ---- 内部の部品（テスト用に公開）

// 1 ファイルを、一時ファイル → 検証 → 名前の変更の順でコピーする。更新日時も引き継ぐ。
// dest がすでにあれば Error（上書きしない）。失敗・キャンセルのときは一時ファイルを消す
void copy_file_verified(const std::filesystem::path& src, const std::filesystem::path& dest, bool verify,
                        const std::atomic<bool>* cancel = nullptr, int64_t* bytes_done = nullptr);

} // namespace focal
