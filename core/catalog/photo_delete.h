#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace focal {

class Catalog;

// 写真の削除（v3.19、design.md 5.11 章）。ディスク上のファイルも消す。利用者の明示的な操作（⌥⌃⇧ Delete）だけが呼ぶ。
//
// 1 枚の写真 = カタログの RAW と、同じフォルダで同じ名前の幹（大文字小文字を問わない）の JPEG・動画・サイドカー。
// ローカルのボリュームのファイルはゴミ箱へ送る（OS の操作なので、呼び出し側が trash で渡す）。
// ネットワークボリューム（SMB・NFS など）はゴミ箱が使えないことが多いので、確認のうえ即座に完全削除する。

struct DeleteItem {
    int64_t photo_id = 0;
    std::filesystem::path raw;                  // 実際に開けるパス。ファイルがなければ空（すでにない）
    std::vector<std::filesystem::path> companions;
    bool network = false;                       // ネットワークボリューム（完全削除になる）
};

struct DeletePlan {
    std::vector<DeleteItem> items;
    int files = 0;            // 消すファイルの数（RAW + 同時に消すもの。すでにないものは数えない）
    int network_photos = 0;   // 完全削除になる写真の数
    int missing_photos = 0;   // ファイルがすでにない写真の数（カタログの情報だけ消える）
};

DeletePlan plan_delete(Catalog& catalog, std::span<const int64_t> photo_ids);

struct DeleteResult {
    int photos_deleted = 0;   // ファイルを消し、カタログから消した写真
    int photos_failed = 0;    // RAW を消せなかったので、カタログに残した写真
    int files_trashed = 0;
    int files_removed = 0;    // 完全に削除したファイル
    int files_failed = 0;     // 同時に消すファイルの失敗も含む
    std::vector<std::string> errors;
};

// ゴミ箱へ送る。成功なら true。null なら、ローカルのファイルは消さずに失敗として扱う（安全側）
using TrashFn = std::function<bool(const std::filesystem::path&)>;

// RAW を消せた写真だけ、カタログから消す（★・フラグ・タグ・アルバムの所属・編集も消える）。
// RAW を消せなかった写真は、ファイルもカタログも残す。同時に消すファイルの失敗は記録して続ける
DeleteResult delete_photos(Catalog& catalog, const DeletePlan& plan, const TrashFn& trash);

} // namespace focal
