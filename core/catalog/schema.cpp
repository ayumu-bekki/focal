#include "catalog/schema.h"

#include <chrono>
#include <ctime>

#include "util/error.h"
#include "util/file.h"

namespace focal::db {

namespace {

constexpr const char* kSchemaV1 = R"SQL(
CREATE TABLE roots (
    id    INTEGER PRIMARY KEY,
    path  TEXT NOT NULL UNIQUE,
    label TEXT
);

CREATE TABLE folders (
    id        INTEGER PRIMARY KEY,
    root_id   INTEGER NOT NULL REFERENCES roots(id) ON DELETE CASCADE,
    parent_id INTEGER REFERENCES folders(id) ON DELETE CASCADE,
    rel_path  TEXT NOT NULL,
    UNIQUE (root_id, rel_path)
);

CREATE TABLE photos (
    id            INTEGER PRIMARY KEY,
    folder_id     INTEGER NOT NULL REFERENCES folders(id) ON DELETE CASCADE,
    file_name     TEXT NOT NULL,
    file_size     INTEGER NOT NULL,
    file_mtime    INTEGER NOT NULL,
    quick_hash    TEXT,
    status        INTEGER NOT NULL DEFAULT 0,
    capture_time  TEXT,
    camera_make   TEXT,
    camera_model  TEXT,
    lens_model    TEXT,
    iso           INTEGER,
    exposure_time REAL,
    f_number      REAL,
    focal_length  REAL,
    width         INTEGER,
    height        INTEGER,
    orientation   INTEGER NOT NULL DEFAULT 0,
    rating        INTEGER NOT NULL DEFAULT 0 CHECK (rating BETWEEN 0 AND 5),
    flag          INTEGER NOT NULL DEFAULT 0 CHECK (flag IN (-1, 0, 1)),
    imported_at   TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    UNIQUE (folder_id, file_name)
);

CREATE TABLE tags (
    id        INTEGER PRIMARY KEY,
    parent_id INTEGER REFERENCES tags(id) ON DELETE CASCADE,
    name      TEXT NOT NULL
);
CREATE UNIQUE INDEX idx_tags_child ON tags(parent_id, name) WHERE parent_id IS NOT NULL;
CREATE UNIQUE INDEX idx_tags_root  ON tags(name)            WHERE parent_id IS NULL;

CREATE TABLE photo_tags (
    photo_id INTEGER NOT NULL REFERENCES photos(id) ON DELETE CASCADE,
    tag_id   INTEGER NOT NULL REFERENCES tags(id)   ON DELETE CASCADE,
    PRIMARY KEY (photo_id, tag_id)
);

CREATE TABLE edits (
    photo_id        INTEGER PRIMARY KEY REFERENCES photos(id) ON DELETE CASCADE,
    process_version INTEGER NOT NULL,
    settings        TEXT NOT NULL,
    updated_at      TEXT NOT NULL
);

CREATE INDEX idx_photos_folder  ON photos(folder_id);
CREATE INDEX idx_photos_capture ON photos(capture_time);
CREATE INDEX idx_photos_rating  ON photos(rating);
CREATE INDEX idx_photos_flag    ON photos(flag);
CREATE INDEX idx_photo_tags_tag ON photo_tags(tag_id);
)SQL";

std::string timestamp_for_filename() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

std::string sql_quote(const std::string& s) {
    std::string r = "'";
    for (char c : s) {
        if (c == '\'') r += '\'';
        r += c;
    }
    return r + "'";
}

// v3.16: アルバム（利用者が選んだ写真の集まり）と、最近の取り込みの時刻などを持つ表
constexpr const char* kSchemaV2 = R"SQL(
CREATE TABLE albums (
    id         INTEGER PRIMARY KEY,
    name       TEXT NOT NULL UNIQUE,
    sort_order INTEGER NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);
CREATE TABLE album_photos (
    album_id INTEGER NOT NULL REFERENCES albums(id) ON DELETE CASCADE,
    photo_id INTEGER NOT NULL REFERENCES photos(id) ON DELETE CASCADE,
    added_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    PRIMARY KEY (album_id, photo_id)
);
CREATE INDEX idx_album_photos_photo ON album_photos(photo_id);
CREATE TABLE meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
)SQL";

// v3.19: アルバムのフォルダ分け・スマートアルバム・カバー、ルートのボリューム ID。
// albums は名前の UNIQUE を親ごとの一意に変えるため作り直す（album_photos を先に消してから albums を消す）
constexpr const char* kSchemaV3 = R"SQL(
CREATE TABLE albums_v3 (
    id             INTEGER PRIMARY KEY,
    parent_id      INTEGER REFERENCES albums_v3(id) ON DELETE CASCADE,
    kind           INTEGER NOT NULL DEFAULT 0 CHECK (kind IN (0, 1, 2)),
    name           TEXT NOT NULL,
    sort_order     INTEGER NOT NULL DEFAULT 0,
    created_at     TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    cover_photo_id INTEGER REFERENCES photos(id) ON DELETE SET NULL,
    query          TEXT
);
INSERT INTO albums_v3 (id, name, sort_order, created_at) SELECT id, name, sort_order, created_at FROM albums;
CREATE TABLE album_photos_v3 (
    album_id INTEGER NOT NULL REFERENCES albums_v3(id) ON DELETE CASCADE,
    photo_id INTEGER NOT NULL REFERENCES photos(id) ON DELETE CASCADE,
    added_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    PRIMARY KEY (album_id, photo_id)
);
INSERT INTO album_photos_v3 (album_id, photo_id, added_at) SELECT album_id, photo_id, added_at FROM album_photos;
DROP TABLE album_photos;
DROP TABLE albums;
ALTER TABLE albums_v3 RENAME TO albums;
ALTER TABLE album_photos_v3 RENAME TO album_photos;
CREATE INDEX idx_album_photos_photo ON album_photos(photo_id);
CREATE INDEX idx_albums_parent ON albums(parent_id);
CREATE UNIQUE INDEX idx_albums_child ON albums(parent_id, name) WHERE parent_id IS NOT NULL;
CREATE UNIQUE INDEX idx_albums_root  ON albums(name)            WHERE parent_id IS NULL;

ALTER TABLE roots ADD COLUMN volume_id       TEXT;
ALTER TABLE roots ADD COLUMN volume_name     TEXT;
ALTER TABLE roots ADD COLUMN volume_rel_path TEXT;
)SQL";

} // namespace

// v3.19: コピーの検出（同じ内容のファイルを探す）を速くする
constexpr const char* kSchemaV4 = R"SQL(
CREATE INDEX IF NOT EXISTS idx_photos_hash ON photos(quick_hash, file_size);
)SQL";

// v3.22: RAW 以外の写真（JPEG・TIFF・PNG・HEIF）。kind は主役のファイルの種類（photo_file.h の PhotoKind。0 = RAW）。
// companions は同じ名前（拡張子を除く）の付属の写真ファイル（RAW の JPEG など）。ファイル名を '/' で区切る（なければ NULL）
constexpr const char* kSchemaV5 = R"SQL(
ALTER TABLE photos ADD COLUMN kind INTEGER NOT NULL DEFAULT 0;
ALTER TABLE photos ADD COLUMN companions TEXT;
)SQL";

const std::vector<Migration>& catalog_migrations() {
    static const std::vector<Migration> m = {{1, kSchemaV1}, {2, kSchemaV2}, {3, kSchemaV3}, {4, kSchemaV4},
                                             {5, kSchemaV5}};
    return m;
}

int latest_schema_version() { return catalog_migrations().back().to_version; }

std::filesystem::path migrate(Database& db, const std::filesystem::path& db_path,
                              const std::vector<Migration>& migrations) {
    const int current = db.user_version();
    const int target = migrations.empty() ? 0 : migrations.back().to_version;
    if (current > target)
        throw Error(Error::Code::Unsupported, "catalog schema " + std::to_string(current) +
                                                  " is newer than this app supports (" + std::to_string(target) + ")");
    if (current == target) return {};

    std::filesystem::path backup;
    if (current > 0) {
        backup = db_path;
        backup += ".v" + std::to_string(current) + "-" + timestamp_for_filename() + ".bak";
        // VACUUM INTO は WAL の内容も含めた一貫したコピーを作る
        db.exec("VACUUM INTO " + sql_quote(path_to_utf8(backup)) + ";");
    } else {
        db.exec("PRAGMA journal_mode = WAL;");
    }

    for (const auto& m : migrations) {
        if (m.to_version <= current) continue;
        Transaction tx(db);
        db.exec(m.sql);
        db.set_user_version(m.to_version);
        tx.commit();
    }
    return backup;
}

} // namespace focal::db
