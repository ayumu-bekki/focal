#include <catch2/catch_test_macros.hpp>

#include "catalog/db_writer.h"
#include "catalog/catalog.h"
#include "catalog/schema.h"
#include "catalog/sqlite.h"
#include "test_util.h"
#include "util/error.h"

using namespace focal;
using namespace focal::db;
namespace fs = std::filesystem;

TEST_CASE("新しいカタログは最新のスキーマ・WAL・外部キー有効で作られる", "[schema]") {
    TempDir dir;
    const fs::path path = dir / "c.sqlite";
    { DbWriter w(path); }
    Database db(path, Database::Mode::ReadOnly);
    CHECK(db.user_version() == latest_schema_version());
    auto fk = db.prepare("PRAGMA foreign_keys;");
    fk.step();
    CHECK(fk.column_int(0) == 1);
    auto jm = db.prepare("PRAGMA journal_mode;");
    jm.step();
    CHECK(jm.column_text(0) == "wal");
    for (const char* t : {"roots", "folders", "photos", "tags", "photo_tags", "edits"}) {
        auto st = db.prepare("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = ?");
        st.bind(1, t).step();
        CHECK(st.column_int(0) == 1);
    }
}

TEST_CASE("タグ名はルート直下でも同じ親の下でも重複できない", "[schema]") {
    TempDir dir;
    const fs::path path = dir / "c.sqlite";
    DbWriter w(path);
    w.call([](Database& db) { db.exec("INSERT INTO tags (parent_id, name) VALUES (NULL, 'A');"); });
    CHECK_THROWS_AS(w.call([](Database& db) { db.exec("INSERT INTO tags (parent_id, name) VALUES (NULL, 'A');"); }),
                    Error);
    w.call([](Database& db) { db.exec("INSERT INTO tags (parent_id, name) VALUES (1, 'B');"); });
    CHECK_THROWS_AS(w.call([](Database& db) { db.exec("INSERT INTO tags (parent_id, name) VALUES (1, 'B');"); }),
                    Error);
}

TEST_CASE("マイグレーション: 順に適用し、既存のカタログは事前にバックアップする", "[schema]") {
    TempDir dir;
    const fs::path path = dir / "m.sqlite";
    const std::vector<Migration> v1 = {{1, "CREATE TABLE t (a INTEGER); INSERT INTO t VALUES (1);"}};
    const std::vector<Migration> v2 = {v1[0], {2, "ALTER TABLE t ADD COLUMN b TEXT; UPDATE t SET b = 'x';"}};

    {
        Database db(path, Database::Mode::ReadWrite);
        CHECK(migrate(db, path, v1).empty());  // 新規作成はバックアップ不要
        CHECK(db.user_version() == 1);
        CHECK(migrate(db, path, v1).empty());  // 最新なら何もしない
    }
    fs::path backup;
    {
        Database db(path, Database::Mode::ReadWrite);
        backup = migrate(db, path, v2);
        CHECK(db.user_version() == 2);
        auto st = db.prepare("SELECT b FROM t");
        REQUIRE(st.step());
        CHECK(st.column_text(0) == "x");
    }
    REQUIRE(!backup.empty());
    REQUIRE(fs::exists(backup));
    Database old(backup, Database::Mode::ReadOnly);
    CHECK(old.user_version() == 1);
    CHECK_THROWS(old.prepare("SELECT b FROM t"));  // バックアップは移行前の内容

    // アプリより新しいカタログは開かない
    Database db(path, Database::Mode::ReadWrite);
    CHECK_THROWS_AS(migrate(db, path, v1), Error);
}

TEST_CASE("書き込みスレッド: 失敗したジョブだけが巻き戻る", "[schema]") {
    TempDir dir;
    const fs::path path = dir / "w.sqlite";
    DbWriter w(path);
    w.call([](Database& db) { db.exec("CREATE TABLE x (v INTEGER UNIQUE);"); });

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 1000; ++i) {
        futures.push_back(w.post([i](Database& db) {
            db.exec("INSERT INTO x VALUES (" + std::to_string(i) + ");");
            if (i == 500) throw Error(Error::Code::Internal, "boom");
        }));
    }
    futures.push_back(w.post([](Database& db) { db.exec("INSERT INTO x VALUES (1);"); }));  // UNIQUE 違反
    int failed = 0;
    for (auto& f : futures) {
        try {
            f.get();
        } catch (const Error&) {
            ++failed;
        }
    }
    CHECK(failed == 2);
    const int count = w.call([](Database& db) {
        auto st = db.prepare("SELECT COUNT(*) FROM x");
        st.step();
        return st.column_int(0);
    });
    CHECK(count == 999);
}

TEST_CASE("マイグレーション: v1 のカタログを開くとアルバムの表が足され、既存のデータは残る", "[schema]") {
    TempDir dir;
    const fs::path path = dir / "c.sqlite";
    {
        Database db(path, Database::Mode::ReadWrite);
        migrate(db, path, {catalog_migrations()[0]});  // v1 のまま作る
        db.exec("INSERT INTO roots (path) VALUES ('/photos'); INSERT INTO tags (name) VALUES ('t');");
    }
    auto c = Catalog::open(path);
    CHECK_FALSE(c->migration_backup().empty());  // 移行前にバックアップ
    CHECK(c->roots().size() == 1);
    CHECK(c->tags().size() == 1);
    CHECK(c->albums().empty());
    c->create_album("A");
    CHECK(c->albums().size() == 1);
    CHECK(latest_schema_version() == 4);
}

TEST_CASE("マイグレーション: v2 のアルバムは v3 でも写真ごと残り、フォルダ・スマートアルバム・ボリュームの列が使える", "[schema]") {
    TempDir dir;
    const fs::path path = dir / "c.sqlite";
    {
        Database db(path, Database::Mode::ReadWrite);
        migrate(db, path, {catalog_migrations()[0], catalog_migrations()[1]});  // v2 のまま作る
        db.exec("INSERT INTO roots (path) VALUES ('/photos');"
                "INSERT INTO folders (root_id, parent_id, rel_path) VALUES (1, NULL, '');"
                "INSERT INTO photos (folder_id, file_name, file_size, file_mtime) VALUES (1, 'a.ARW', 1, 1);"
                "INSERT INTO albums (name, sort_order) VALUES ('Trip', 1), ('Best', 2);"
                "INSERT INTO album_photos (album_id, photo_id) VALUES (1, 1), (2, 1);");
    }
    auto c = Catalog::open(path);
    CHECK_FALSE(c->migration_backup().empty());
    const auto albums = c->albums();
    REQUIRE(albums.size() == 2);
    CHECK(albums[0].name == "Trip");
    CHECK(albums[0].kind == AlbumKind::Album);
    CHECK(albums[0].photo_count == 1);
    CHECK_FALSE(albums[0].parent_id.has_value());
    CHECK(c->roots().size() == 1);

    // 作り直した表でも外部キーが生きている（アルバムを消すと所属も消え、写真は残る）
    c->delete_album(albums[0].id);
    CHECK(c->albums().size() == 1);
    CHECK(c->count(PhotoFilter{}) == 1);
    Database db(path, Database::Mode::ReadOnly);
    auto st = db.prepare("SELECT COUNT(*) FROM album_photos");
    st.step();
    CHECK(st.column_int(0) == 1);
}
