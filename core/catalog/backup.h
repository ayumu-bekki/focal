#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace focal {

// カタログの定期バックアップ（v3.24、design.md 7.1 章）。操作の前の安全バックアップ（Catalog::backup。カタログの
// パッケージの中の *.bak）とは別に、利用者が選んだフォルダへ、**そのまま開けるカタログのパッケージ**
// （<カタログ名> YYYY-MM-DD HHMM.focalcatalog）として保存する。復元は「カタログを開く」で開くだけ。

struct BackupOptions {
    bool check_integrity = true;  // 取る前に PRAGMA quick_check を行う
    bool allow_damaged = false;   // 整合性の確認に失敗しても取る（取らずに知らせたあと、利用者が「それでも取る」を選んだとき）
    std::function<void(double)> progress;   // 0..1。書いているファイルの大きさ ÷ 元の大きさ。別のスレッドから呼ぶ
    const std::atomic<bool>* cancel = nullptr;  // true になったら打ち切り、書きかけを消して Error(Cancelled) を投げる
};

struct BackupResult {
    std::filesystem::path path;       // 作ったバックアップ（取らなかったときは空）
    int64_t bytes = 0;                // catalog.sqlite の大きさ
    bool skipped_damaged = false;     // 整合性の確認に失敗したので取らなかった
    std::string integrity_message;    // 失敗したときの SQLite の報告（先頭の数行）
};

struct BackupInfo {
    std::filesystem::path path;  // …/<名前> 2026-10-05 2130.focalcatalog
    std::string created;         // "YYYY-MM-DD HH:MM"（名前から）
    int64_t bytes = 0;
};

// dir の中の、catalog_name のバックアップを新しい順に返す。dir がなければ空
std::vector<BackupInfo> list_backups(const std::filesystem::path& dir, const std::string& catalog_name);

// 新しい keep 個を残して、古いバックアップを消す。keep <= 0 なら何も消さない。消した数を返す
int prune_backups(const std::filesystem::path& dir, const std::string& catalog_name, int keep);

} // namespace focal
