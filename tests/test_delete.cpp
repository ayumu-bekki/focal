// v3.19: 写真の削除（RAW + 同じ名前の幹の JPEG・サイドカーをゴミ箱へ、ネットワークは完全削除）
#include <catch2/catch_test_macros.hpp>

#include <fstream>

#include "catalog/catalog.h"
#include "catalog/photo_delete.h"
#include "test_util.h"
#include "util/error.h"
#include "util/file.h"

using namespace focal;
namespace fs = std::filesystem;

namespace {

fs::path data_file(const char* name) { return fs::path(FOCAL_TEST_DATA_DIR) / name; }

bool have_data() { return fs::exists(data_file("sony_ilce7m3.ARW")) && fs::exists(data_file("canon_eos_m50.CR3")); }

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}

// root/2018/A.ARW + A.JPG + a.xmp（名前の幹は大文字小文字を問わない）+ AB.JPG（別の写真の JPEG）、root/2018/B.CR3 + B.xmp
struct Lib {
    TempDir dir{"focal-del"};
    fs::path root = dir / "photos";
    fs::path trash = dir / "trash";

    Lib() {
        fs::create_directories(root / "2018");
        fs::create_directories(trash);
        fs::copy_file(data_file("sony_ilce7m3.ARW"), root / "2018" / "A.ARW");
        fs::copy_file(data_file("canon_eos_m50.CR3"), root / "2018" / "B.CR3");
        write_file(root / "2018" / "A.JPG", "jpeg");
        write_file(root / "2018" / "a.xmp", "xmp");
        write_file(root / "2018" / "AB.JPG", "another photo's jpeg");
        write_file(root / "2018" / "B.xmp", "xmp");
        write_file(root / "2018" / "notes.txt", "x");
    }

    // ゴミ箱の代わり: trash フォルダへ移す
    TrashFn move_to_trash(std::vector<std::string>* log = nullptr) {
        return [this, log](const fs::path& p) {
            if (log) log->push_back(path_to_utf8(p.filename()));
            std::error_code ec;
            fs::rename(p, trash / p.filename(), ec);
            return !ec;
        };
    }
};

int64_t id_of(Catalog& c, const std::string& name) {
    for (const auto& p : c.query(PhotoFilter{}))
        if (p.file_name == name) return p.id;
    FAIL("no photo " << name);
    return 0;
}

} // namespace

TEST_CASE("削除の計画: 同じフォルダで名前の幹が同じ JPEG・サイドカーだけを一緒に消す", "[delete][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Lib lib;
    TempDir dir;
    auto c = Catalog::open(dir / "c.sqlite");
    c->scan_root(c->add_root(lib.root));
    const int64_t a = id_of(*c, "A.ARW");
    const std::vector<int64_t> ids = {a};
    const DeletePlan plan = plan_delete(*c, ids);
    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items[0].companions.size() == 2);  // A.JPG と a.xmp。AB.JPG・notes.txt は含まない
    CHECK(plan.files == 3);
    CHECK(plan.network_photos == 0);
    CHECK(plan.missing_photos == 0);
    CHECK(plan_delete(*c, std::vector<int64_t>{99999}).items.empty());  // カタログにない id は飛ばす
}

TEST_CASE("削除: ファイルをゴミ箱へ送り、カタログの情報（アルバム・タグ）も消す", "[delete][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Lib lib;
    TempDir dir;
    auto c = Catalog::open(dir / "c.sqlite");
    c->scan_root(c->add_root(lib.root));
    const int64_t a = id_of(*c, "A.ARW"), b = id_of(*c, "B.CR3");
    const int64_t album = c->create_album("Keep");
    c->add_to_album(album, std::vector<int64_t>{a, b});
    c->add_tag(std::vector<int64_t>{a}, c->ensure_tag("t"));
    c->set_album_cover(album, a);

    std::vector<std::string> log;
    const DeletePlan plan = plan_delete(*c, std::vector<int64_t>{a});
    const DeleteResult r = delete_photos(*c, plan, lib.move_to_trash(&log));
    CHECK(r.photos_deleted == 1);
    CHECK(r.photos_failed == 0);
    CHECK(r.files_trashed == 3);
    CHECK(r.files_removed == 0);
    CHECK(r.errors.empty());
    CHECK(log.size() == 3);
    CHECK_FALSE(fs::exists(lib.root / "2018" / "A.ARW"));
    CHECK_FALSE(fs::exists(lib.root / "2018" / "A.JPG"));
    CHECK_FALSE(fs::exists(lib.root / "2018" / "a.xmp"));
    CHECK(fs::exists(lib.trash / "A.ARW"));
    // 関係ないファイルは残る
    CHECK(fs::exists(lib.root / "2018" / "AB.JPG"));
    CHECK(fs::exists(lib.root / "2018" / "notes.txt"));
    CHECK(fs::exists(lib.root / "2018" / "B.CR3"));
    // カタログ: A だけ消え、アルバムの所属・カバー・タグも消える
    CHECK(c->photo(a) == std::nullopt);
    CHECK(c->photo(b).has_value());
    CHECK(c->count(PhotoFilter{}) == 2);  // B と、別の名前の AB.JPG（JPEG だけの 1 枚。v3.22）
    PhotoFilter f;
    f.album_id = album;
    CHECK(c->count(f) == 1);
    CHECK(c->albums()[0].cover_photo_id == b);  // カバーの指定は外れ、残った写真になる
    CHECK(c->tags()[0].photo_count == 0);
}

TEST_CASE("削除: ゴミ箱へ送れなかった RAW は、ファイルもカタログも残す", "[delete][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Lib lib;
    TempDir dir;
    auto c = Catalog::open(dir / "c.sqlite");
    c->scan_root(c->add_root(lib.root));
    const int64_t a = id_of(*c, "A.ARW");
    const DeletePlan plan = plan_delete(*c, std::vector<int64_t>{a});

    // ゴミ箱が使えない（trash が false）
    DeleteResult r = delete_photos(*c, plan, [](const fs::path&) { return false; });
    CHECK(r.photos_deleted == 0);
    CHECK(r.photos_failed == 1);
    CHECK(r.errors.size() == 1);
    CHECK(fs::exists(lib.root / "2018" / "A.ARW"));
    CHECK(fs::exists(lib.root / "2018" / "A.JPG"));  // RAW が残るので同時に消すものも触らない
    CHECK(c->photo(a).has_value());

    // trash を渡さなければ、ローカルのファイルは消さない（安全側）
    r = delete_photos(*c, plan, nullptr);
    CHECK(r.photos_failed == 1);
    CHECK(fs::exists(lib.root / "2018" / "A.ARW"));

    // RAW は送れたが JPEG だけ失敗: 写真は消え、失敗を記録する
    int calls = 0;
    r = delete_photos(*c, plan, [&](const fs::path& p) {
        ++calls;
        if (p.extension() == ".JPG") return false;
        std::error_code ec;
        fs::rename(p, lib.trash / p.filename(), ec);
        return !ec;
    });
    CHECK(r.photos_deleted == 1);
    CHECK(r.files_failed == 1);
    CHECK(r.errors.size() == 1);
    CHECK(c->photo(a) == std::nullopt);
    CHECK(fs::exists(lib.root / "2018" / "A.JPG"));
}

TEST_CASE("削除: ネットワークボリューム（ゴミ箱が使えない）は、ゴミ箱を使わず完全に消す", "[delete][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Lib lib;
    TempDir dir;
    auto c = Catalog::open(dir / "c.sqlite");
    c->scan_root(c->add_root(lib.root));
    const int64_t b = id_of(*c, "B.CR3");
    DeletePlan plan = plan_delete(*c, std::vector<int64_t>{b});
    REQUIRE(plan.items.size() == 1);
    plan.items[0].network = true;  // ネットワークの共有にあることにする（この環境にはないので）
    plan.network_photos = 1;
    bool trash_called = false;
    const DeleteResult r = delete_photos(*c, plan, [&](const fs::path&) {
        trash_called = true;
        return true;
    });
    CHECK_FALSE(trash_called);
    CHECK(r.photos_deleted == 1);
    CHECK(r.files_removed == 2);  // B.CR3 と B.xmp
    CHECK(r.files_trashed == 0);
    CHECK_FALSE(fs::exists(lib.root / "2018" / "B.CR3"));
    CHECK_FALSE(fs::exists(lib.root / "2018" / "B.xmp"));
    CHECK(c->photo(b) == std::nullopt);
}

TEST_CASE("削除: ファイルがすでにない写真は、カタログの情報だけ消す", "[delete][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Lib lib;
    TempDir dir;
    auto c = Catalog::open(dir / "c.sqlite");
    c->scan_root(c->add_root(lib.root));
    const int64_t a = id_of(*c, "A.ARW");
    fs::remove(lib.root / "2018" / "A.ARW");
    const DeletePlan plan = plan_delete(*c, std::vector<int64_t>{a});
    CHECK(plan.missing_photos == 1);
    CHECK(plan.files == 0);
    const DeleteResult r = delete_photos(*c, plan, lib.move_to_trash());
    CHECK(r.photos_deleted == 1);
    CHECK(c->photo(a) == std::nullopt);
    CHECK(fs::exists(lib.root / "2018" / "A.JPG"));  // RAW がないので、JPEG には触れない
}
