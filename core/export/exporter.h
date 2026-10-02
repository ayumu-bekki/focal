#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace focal {

class Catalog;

// 書き出し（5.8 章）。sRGB、ICC 埋め込み、EXIF なし（14 章）。
struct ExportOptions {
    enum class Format { Jpeg, Tiff16 };
    Format format = Format::Jpeg;
    int quality = 92;    // JPEG
    int long_edge = 0;   // 0 なら原寸。それ以外は長辺をこの画素数に縮める（Lanczos3）
    std::filesystem::path dest_dir;
};

struct ExportItemResult {
    int64_t photo_id = 0;
    bool ok = false;
    std::filesystem::path output;  // 成功時
    std::string error;             // 失敗時
};

// 書き出し先の名前を確保する: {元のファイル名から拡張子を除いたもの}.{ext}。
// あれば _1, _2 … を付ける。新規作成のみで確保する（上書きしない、同時に書き出しても重ならない）
std::filesystem::path reserve_output_path(const std::filesystem::path& dir, const std::string& stem_nfc,
                                          const std::string& ext);

// 1 枚を書き出す。保存済みの編集を反映する。cancel が立てばデコードの途中でも打ち切る（Error(Cancelled)）
ExportItemResult export_photo(Catalog& catalog, int64_t photo_id, const ExportOptions& options,
                              const std::atomic<bool>* cancel = nullptr);

// 複数枚を順に書き出す（1 枚ずつ。各写真のレンダリングは内部で並列化される）。
// progress(done, total, 結果) を 1 枚ごとに呼ぶ。キャンセルされたら残りは書き出さずに戻る（戻り値 false）
bool export_photos(Catalog& catalog, const std::vector<int64_t>& ids, const ExportOptions& options,
                   const std::function<void(int done, int total, const ExportItemResult&)>& progress,
                   const std::atomic<bool>* cancel = nullptr);

} // namespace focal
