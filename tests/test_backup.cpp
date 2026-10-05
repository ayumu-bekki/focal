// v3.24: カタログの定期バックアップ（開けるカタログのパッケージ・世代の整理・前回日時と判定・キャンセル）
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <fstream>

#include "catalog/backup.h"
#include "catalog/catalog.h"
#include "catalog/sqlite.h"
#include "test_util.h"
#include "util/error.h"
#include "util/file.h"

using namespace focal;
namespace fs = std::filesystem;

namespace {

// 写真の行を 1 つ持つカタログ（中身の見えないファイルでも行は作られる）
std::unique_ptr<Catalog> make_catalog(const fs::path& db, const fs::path& lib) {
    fs::create_directories(lib);
    std::ofstream(lib / "a.NEF") << "x";
    auto c = Catalog::open(db);
    c->scan_root(c->add_root(lib));
    c->set_rating(std::vector<int64_t>{c->query(PhotoFilter{})[0].id}, 4);
    c->flush();
    return c;
}

} // namespace

TEST_CASE("バックアップ: 開けるカタログのパッケージとして保存し、前回の日時を記録する", "[backup]") {
    TempDir lib, dir, dest;
    auto c = make_catalog(dir / "My Catalog.focalcatalog" / "catalog.sqlite", lib.path());
    CHECK(c->name() == "My Catalog");
    CHECK_FALSE(c->last_backup_at());

    std::vector<double> progress;
    BackupOptions opt;
    opt.progress = [&](double p) { progress.push_back(p); };
    const BackupResult r = c->backup_to(dest.path(), opt);
    REQUIRE_FALSE(r.skipped_damaged);
    REQUIRE_FALSE(r.path.empty());
    CHECK(r.path.extension() == ".focalcatalog");
    CHECK(path_to_utf8(r.path.filename()).rfind("My Catalog ", 0) == 0);
    CHECK(r.bytes > 0);
    CHECK_FALSE(progress.empty());
    CHECK(progress.back() == 1.0);
    for (double p : progress) CHECK((p >= 0.0 && p <= 1.0));
    // 途中の一時フォルダは残らない
    for (const auto& e : fs::directory_iterator(dest.path())) CHECK(e.path().extension() == ".focalcatalog");

    // バックアップは、そのまま開けて、同じ内容
    auto b = Catalog::open(r.path / "catalog.sqlite");
    REQUIRE(b->query(PhotoFilter{}).size() == 1);
    CHECK(b->query(PhotoFilter{})[0].rating == 4);
    CHECK(b->name() == "My Catalog " + path_to_utf8(r.path.filename()).substr(11, 15));  // バックアップ自身の名前

    // 前回の日時が記録される（UTC の ISO 形式）
    const auto last = c->last_backup_at();
    REQUIRE(last);
    CHECK(last->size() == 20);
    CHECK(last->back() == 'Z');

    // 同じ分にもう一度取ると、連番が付く
    const BackupResult r2 = c->backup_to(dest.path());
    CHECK(r2.path != r.path);
    CHECK(fs::exists(r2.path / "catalog.sqlite"));
    CHECK(list_backups(dest.path(), "My Catalog").size() == 2);
}

TEST_CASE("バックアップ: 前回から指定の日数がたったかの判定（0 は毎回、負は自動なし）", "[backup]") {
    TempDir lib, dir, dest;
    auto c = make_catalog(dir / "c.sqlite", lib.path());
    CHECK(c->name() == "c");
    // 一度も取っていなければ、日数の指定があれば取る時期。自動なしは常に取らない
    CHECK(c->backup_due(7));
    CHECK(c->backup_due(0));
    CHECK_FALSE(c->backup_due(-1));

    c->backup_to(dest.path());
    CHECK_FALSE(c->backup_due(1));
    CHECK_FALSE(c->backup_due(7));
    CHECK(c->backup_due(0));  // 毎回

    // 前回を 10 日前にする（別の接続で書き換える）
    {
        db::Database db(dir / "c.sqlite", db::Database::Mode::ReadWrite);
        const std::time_t t = std::time(nullptr) - 10 * 86400;
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
        db.prepare("UPDATE meta SET value = ? WHERE key = 'last_backup_at'").bind(1, buf).run();
    }
    CHECK(c->backup_due(7));
    CHECK(c->backup_due(10));
    CHECK_FALSE(c->backup_due(14));
    CHECK_FALSE(c->backup_due(30));
}

TEST_CASE("バックアップ: 一覧は新しい順、世代の整理は新しい N 個を残し、関係のないものには触れない", "[backup]") {
    TempDir dest;
    auto make = [&](const std::string& name) {
        fs::create_directories(dest / name);
        std::ofstream(dest / name / "catalog.sqlite") << "db";
    };
    make("Cat 2026-10-01 0900.focalcatalog");
    make("Cat 2026-10-02 0900.focalcatalog");
    make("Cat 2026-10-03 0900.focalcatalog");
    make("Cat 2026-10-03 0900 2.focalcatalog");
    make("Other 2026-10-01 0900.focalcatalog");    // 別のカタログのバックアップ
    make("Cat Archive.focalcatalog");              // 名前が合わない
    fs::create_directories(dest / "Cat 2026-10-04 0900.focalcatalog.partial");  // 途中のもの
    std::ofstream(dest / "notes.txt") << "keep me";

    const auto list = list_backups(dest.path(), "Cat");
    REQUIRE(list.size() == 4);
    CHECK(list[0].created == "2026-10-03 09:00");
    CHECK(path_to_utf8(list[0].path.filename()) == "Cat 2026-10-03 0900 2.focalcatalog");  // 連番のあとが新しい
    CHECK(list[3].created == "2026-10-01 09:00");
    CHECK(list_backups(dest.path(), "Missing").empty());
    CHECK(list_backups(dest / "no-such-dir", "Cat").empty());

    CHECK(prune_backups(dest.path(), "Cat", 0) == 0);  // 0 はすべて残す
    CHECK(prune_backups(dest.path(), "Cat", 10) == 0);
    CHECK(prune_backups(dest.path(), "Cat", 2) == 2);
    const auto after = list_backups(dest.path(), "Cat");
    REQUIRE(after.size() == 2);
    CHECK(after[1].created == "2026-10-03 09:00");
    CHECK(fs::exists(dest / "Other 2026-10-01 0900.focalcatalog"));
    CHECK(fs::exists(dest / "Cat Archive.focalcatalog"));
    CHECK(fs::exists(dest / "notes.txt"));
}

TEST_CASE("バックアップ: キャンセルすると書きかけを残さず、前回の日時も変えない", "[backup]") {
    TempDir lib, dir, dest;
    auto c = make_catalog(dir / "c.sqlite", lib.path());
    std::atomic<bool> cancel{true};
    BackupOptions opt;
    opt.cancel = &cancel;
    CHECK_THROWS_AS(c->backup_to(dest.path(), opt), Error);
    CHECK(fs::is_empty(dest.path()));
    CHECK_FALSE(c->last_backup_at());
    // 保存先を作れなければ Io エラー
    std::ofstream(dest / "file") << "x";
    CHECK_THROWS_AS(c->backup_to(dest / "file" / "sub"), Error);
}

TEST_CASE("カタログごとの設定: カタログに保存され、開き直しても残り、不正な名前は拒否する", "[prefs]") {
    TempDir dir;
    {
        auto c = Catalog::open(dir / "c.sqlite");
        CHECK_FALSE(c->preference("import_destination"));
        c->set_preference("import_destination", std::string("/Volumes/Photos"));
        c->set_preference("rescan_on_launch", std::string("0"));
        CHECK(c->preference("import_destination") == "/Volumes/Photos");
        c->set_preference("import_destination", std::string("/other"));  // 上書き
        CHECK(c->preference("import_destination") == "/other");
        CHECK_THROWS_AS(c->preference("Bad Name"), Error);
        // 内部のキー（last_import_at など）とは、`pref.` の接頭辞で区別されるので、同じ名前でも衝突しない
        c->set_preference("last_import_at", std::string("x"));
        CHECK(c->preference("last_import_at") == "x");
        CHECK_THROWS_AS(c->preference("with.dot"), Error);
        CHECK_THROWS_AS(c->set_preference("", std::string("x")), Error);
    }
    auto again = Catalog::open(dir / "c.sqlite");
    CHECK(again->preference("import_destination") == "/other");
    CHECK(again->preference("rescan_on_launch") == "0");
    again->set_preference("import_destination", std::nullopt);  // 消すと既定に戻る
    CHECK_FALSE(again->preference("import_destination"));
    // 別のカタログには影響しない
    auto other = Catalog::open(dir / "d.sqlite");
    CHECK_FALSE(other->preference("rescan_on_launch"));
}
