#pragma once

#include <filesystem>
#include <vector>

#include "catalog/sqlite.h"

namespace focal::db {

struct Migration {
    int to_version;  // この SQL を適用した後の user_version
    const char* sql;
};

// 7.2 章のスキーマ（user_version = 1 から）
const std::vector<Migration>& catalog_migrations();
int latest_schema_version();

// user_version を見て、足りない分のマイグレーションを順に適用する（7.1 章）。
// 既存のカタログ（user_version > 0）を更新する前に、db_path の横へバックアップを作る。
// 戻り値はバックアップのパス（作らなかった場合は空）。
std::filesystem::path migrate(Database& db, const std::filesystem::path& db_path,
                              const std::vector<Migration>& migrations);

} // namespace focal::db
