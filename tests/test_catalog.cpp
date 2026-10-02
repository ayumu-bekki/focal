#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <thread>

#include "catalog/catalog.h"
#include "test_util.h"
#include "thumbs/thumbnail.h"
#include "util/error.h"
#include "util/file.h"
#include "util/unicode.h"

using namespace focal;
namespace fs = std::filesystem;

namespace {

const std::string kNfc = "\xe3\x81\x8c\xe3\x81\xa3\xe3\x81\x93\xe3\x81\x86";  // がっこう
const std::string kNfd = "\xe3\x81\x8b\xe3\x82\x99\xe3\x81\xa3\xe3\x81\x93\xe3\x81\x86";
const std::string kTrip = "\xe6\x97\x85\xe8\xa1\x8c";  // 旅行

fs::path data_file(const char* name) { return fs::path(FOCAL_TEST_DATA_DIR) / name; }

bool have_data() { return fs::exists(data_file("sony_ilce7m3.ARW")) && fs::exists(data_file("canon_eos_m50.CR3")); }

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}

// 取り込み用のフォルダ:
//   2018/旅行/がっこう.ARW（NFD の名前）, 2018/IMG_0001.CR3, 2019/broken.NEF（壊れている）,
//   .cache/hidden.ARW（隠しフォルダ）, notes.txt（RAW ではない）
struct Library {
    TempDir dir{"focal-lib"};
    fs::path root = dir / "photos";

    Library() {
        fs::create_directories(root / utf8_to_path("2018/" + kTrip));
        fs::copy_file(data_file("sony_ilce7m3.ARW"), root / utf8_to_path("2018/" + kTrip + "/" + kNfd + ".ARW"));
        fs::copy_file(data_file("canon_eos_m50.CR3"), root / "2018" / "IMG_0001.CR3");
        write_file(root / "2019" / "broken.NEF", "not a raw file");
        write_file(root / ".cache" / "hidden.ARW", "x");
        write_file(root / "notes.txt", "x");
    }
};

std::vector<PhotoRecord> all(Catalog& c) { return c.query(PhotoFilter{}); }

const PhotoRecord* find_by_name(const std::vector<PhotoRecord>& v, const std::string& name) {
    for (const auto& p : v)
        if (p.file_name == name) return &p;
    return nullptr;
}

void bump_mtime(const fs::path& p) {
    fs::last_write_time(p, fs::last_write_time(p) + std::chrono::seconds(10));
}

} // namespace

TEST_CASE("カタログ: 取り込み・メタデータ・NFC", "[catalog][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Library lib;
    TempDir cdir;
    auto c = Catalog::open(cdir / "c.sqlite");
    const int64_t root = c->add_root(lib.root);
    CHECK(c->add_root(lib.root / "") == root);  // 同じフォルダは二重登録しない

    const ScanStats s = c->scan_root(root);
    CHECK(s.added == 3);
    CHECK(s.unsupported == 1);
    CHECK(s.folders_added == 3);  // 2018, 2018/旅行, 2019（ルート自身は登録時に作る）

    const auto photos = all(*c);
    REQUIRE(photos.size() == 3);

    // NFD の名前は NFC で保存される
    const PhotoRecord* sony = find_by_name(photos, kNfc + ".ARW");
    REQUIRE(sony);
    CHECK(sony->path.find(kTrip + "/" + kNfc + ".ARW") != std::string::npos);
    CHECK(sony->camera_make == "Sony");
    CHECK(sony->camera_model == "ILCE-7M3");
    CHECK(sony->iso == 400);
    CHECK(sony->capture_time == "2018-03-13T16:38:13");
    CHECK(sony->width == 6024);
    CHECK(sony->height == 4024);
    CHECK(sony->quick_hash.size() == 64);
    CHECK(sony->status == PhotoStatus::Ok);
    CHECK(sony->lens_model == "FE 16-35mm F2.8 GM");
    REQUIRE(sony->exposure_time);
    CHECK(*sony->exposure_time == 1.0f / 50);  // LibRaw は float

    const auto disk = c->photo_disk_path(sony->id);
    REQUIRE(disk);
    CHECK(fs::exists(*disk));

    const PhotoRecord* broken = find_by_name(photos, "broken.NEF");
    REQUIRE(broken);
    CHECK(broken->status == PhotoStatus::Unsupported);
    CHECK_FALSE(broken->capture_time);

    // 撮影日時順（不明は最後）
    CHECK(photos.front().file_name == kNfc + ".ARW");
    CHECK(photos.back().file_name == "broken.NEF");

    // フォルダ
    const auto folders = c->folders(root);
    REQUIRE(folders.size() == 4);
    CHECK(folders[0].rel_path.empty());
    CHECK(c->folder_id(root, "2018/" + kTrip));
    CHECK(c->folder_id(root, "2018/" + to_nfc(kTrip)));
}

TEST_CASE("カタログ: 再スキャン（変更なし・大文字小文字のリネーム・削除・復帰・更新）", "[catalog][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Library lib;
    TempDir cdir;
    auto c = Catalog::open(cdir / "c.sqlite");
    const int64_t root = c->add_root(lib.root);
    c->scan_root(root);
    const int64_t canon_id = find_by_name(all(*c), "IMG_0001.CR3")->id;
    c->set_rating(std::vector<int64_t>{canon_id}, 4);

    // 変更なし
    ScanStats s = c->scan_root(root);
    CHECK(s.added == 0);
    CHECK(s.unchanged == 3);
    CHECK(s.updated == 0);

    // 大文字小文字だけのリネームは同じ写真のまま（★も引き継ぐ）
    fs::rename(lib.root / "2018" / "IMG_0001.CR3", lib.root / "2018" / "img_0001.cr3.tmp");
    fs::rename(lib.root / "2018" / "img_0001.cr3.tmp", lib.root / "2018" / "img_0001.cr3");
    s = c->scan_root(root);
    CHECK(s.renamed == 1);
    CHECK(s.added == 0);
    CHECK(s.missing == 0);
    auto canon = c->photo(canon_id);
    REQUIRE(canon);
    CHECK(canon->file_name == "img_0001.cr3");
    CHECK(canon->rating == 4);

    // 削除 → ファイルなし（行は残る）
    const fs::path moved = cdir / "away.cr3";
    fs::rename(lib.root / "2018" / "img_0001.cr3", moved);
    s = c->scan_root(root);
    CHECK(s.missing == 1);
    CHECK(c->photo(canon_id)->status == PhotoStatus::Missing);
    CHECK(c->photo(canon_id)->rating == 4);
    PhotoFilter available;
    available.include_unavailable = false;
    CHECK(c->count(available) == 1);

    // 戻す → 復帰
    fs::rename(moved, lib.root / "2018" / "img_0001.cr3");
    s = c->scan_root(root);
    CHECK(s.restored == 1);
    CHECK(c->photo(canon_id)->status == PhotoStatus::Ok);

    // 更新日時が変わったら読み直す
    bump_mtime(lib.root / "2018" / "img_0001.cr3");
    s = c->scan_root(root);
    CHECK(s.updated == 1);
    CHECK(s.unchanged == 2);

    // フォルダ名の大文字小文字が変わっても同じフォルダ
    const auto before = c->folder_id(root, "2018");
    fs::rename(lib.root / "2018", lib.root / "2018x");
    fs::rename(lib.root / "2018x", lib.root / "2018");
    CHECK(c->folder_id(root, "2018") == before);
}

TEST_CASE("カタログ: ルートにアクセスできなければ何も変えない", "[catalog][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Library lib;
    TempDir cdir;
    auto c = Catalog::open(cdir / "c.sqlite");
    const int64_t root = c->add_root(lib.root);
    c->scan_root(root);
    const fs::path away = lib.dir / "unplugged";
    fs::rename(lib.root, away);  // 外付けドライブを外した状態
    CHECK_THROWS_AS(c->scan_root(root), Error);
    PhotoFilter available;
    available.include_unavailable = false;
    CHECK(c->count(available) == 2);  // ファイルなしにはしない
    fs::rename(away, lib.root);
}

TEST_CASE("カタログ: ★・フラグ・タグ・日付・フォルダで絞り込む", "[catalog][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Library lib;
    TempDir cdir;
    auto c = Catalog::open(cdir / "c.sqlite");
    const int64_t root = c->add_root(lib.root);
    c->scan_root(root);
    const auto photos = all(*c);
    const int64_t sony = find_by_name(photos, kNfc + ".ARW")->id;
    const int64_t canon = find_by_name(photos, "IMG_0001.CR3")->id;
    const int64_t broken = find_by_name(photos, "broken.NEF")->id;

    c->set_rating(std::vector<int64_t>{sony, canon}, 3);
    c->set_rating(std::vector<int64_t>{canon}, 5);
    c->set_flag(std::vector<int64_t>{sony}, 1);
    c->set_flag(std::vector<int64_t>{broken}, -1);
    CHECK_THROWS_AS(c->set_rating(std::vector<int64_t>{sony}, 6), Error);

    PhotoFilter f;
    f.min_rating = 3;
    CHECK(c->count(f) == 2);
    f.min_rating = 5;
    CHECK(c->count(f) == 1);

    f = {};
    f.flag = FlagFilter::Picked;
    CHECK(c->count(f) == 1);
    f.flag = FlagFilter::Rejected;
    CHECK(c->count(f) == 1);
    f.flag = FlagFilter::NotRejected;
    CHECK(c->count(f) == 2);
    f.flag = FlagFilter::Unflagged;
    CHECK(c->count(f) == 1);

    // 階層タグ: 親タグで絞り込むと子タグの写真も含む
    const int64_t family = c->ensure_tag("Portrait/" + kNfd);  // NFD で渡しても NFC で保存
    const int64_t portrait = *c->find_tag("Portrait");
    CHECK(c->ensure_tag("Portrait/" + kNfc) == family);
    CHECK(c->find_tag("Portrait/" + kNfc) == family);
    const int64_t landscape = c->ensure_tag("Landscape");
    c->add_tag(std::vector<int64_t>{sony}, family);
    c->add_tag(std::vector<int64_t>{canon}, portrait);
    c->add_tag(std::vector<int64_t>{canon}, landscape);
    c->add_tag(std::vector<int64_t>{canon}, landscape);  // 二重に付けても 1 つ

    f = {};
    f.tag_id = portrait;
    CHECK(c->count(f) == 2);
    f.tag_id = family;
    CHECK(c->count(f) == 1);
    f.tag_id = landscape;
    CHECK(c->count(f) == 1);
    c->remove_tag(std::vector<int64_t>{canon}, landscape);
    CHECK(c->count(f) == 0);

    const auto tags = c->tags();
    REQUIRE(tags.size() == 3);
    CHECK(tags[0].path == "Landscape");
    CHECK(tags[1].path == "Portrait");
    CHECK(tags[2].path == "Portrait/" + kNfc);
    CHECK(tags[2].photo_count == 1);
    CHECK(c->photo_tags(canon).size() == 1);

    // 撮影日（ローカル時刻の文字列で比較）
    f = {};
    f.date_from = "2018-03-13";
    f.date_to = "2018-03-13";
    CHECK(c->count(f) == 1);
    f.date_from = "2018-03-14";
    f.date_to = "2018-12-31";
    CHECK(c->count(f) == 1);

    // フォルダ（既定ではサブフォルダも含む）
    f = {};
    f.folder_id = c->folder_id(root, "2018");
    CHECK(c->count(f) == 2);
    f.include_subfolders = false;
    CHECK(c->count(f) == 1);

    // ページング
    CHECK(c->query({}, 0, 2).size() == 2);
    CHECK(c->query({}, 2, 2).size() == 1);
}

TEST_CASE("カタログ: 取り込み時にサムネイルを作り、なくなったら再スキャンで作り直す", "[catalog][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Library lib;
    TempDir cdir;
    auto c = Catalog::open(cdir / "c.sqlite");
    const ThumbnailCache cache(cdir / "thumbs");
    const int64_t root = c->add_root(lib.root);
    ScanOptions opt;
    opt.thumbnails = &cache;
    ScanStats s = c->scan_root(root, opt);
    CHECK(s.thumbnails == 2);  // 壊れたファイルは作らない
    CHECK(s.thumbnail_failures == 0);

    const PhotoRecord sony = *find_by_name(all(*c), kNfc + ".ARW");
    const std::string key = thumbnail_key(sony.path, sony.file_size, sony.file_mtime);
    REQUIRE(cache.contains(key));

    fs::remove(cache.path_for(key));
    s = c->scan_root(root, opt);
    CHECK(s.thumbnails == 1);
    CHECK(s.unchanged == 3);
    CHECK(cache.contains(key));
}

TEST_CASE("カタログ: 開き直しても内容が残り、読み取りと書き込みが並行できる", "[catalog][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Library lib;
    TempDir cdir;
    int64_t id = 0;
    {
        auto c = Catalog::open(cdir / "c.sqlite");
        c->scan_root(c->add_root(lib.root));
        id = all(*c).front().id;
        c->set_flag(std::vector<int64_t>{id}, 1);
    }
    auto c = Catalog::open(cdir / "c.sqlite");
    CHECK(c->migration_backup().empty());
    CHECK(c->photo(id)->flag == 1);

    std::atomic<bool> stop{false};
    std::thread reader([&] {
        while (!stop) CHECK(c->count({}) == 3);
    });
    for (int i = 0; i < 200; ++i) c->set_rating(std::vector<int64_t>{id}, i % 6);
    stop = true;
    reader.join();
    CHECK(c->photo(id)->rating == 199 % 6);
}

TEST_CASE("カタログ: アルバムの作成・名前・削除と、写真の追加・外す・絞り込み", "[catalog][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Library lib;
    TempDir dir;
    auto c = Catalog::open(dir / "c.sqlite");
    c->scan_root(c->add_root(lib.root));
    const auto photos = all(*c);
    REQUIRE(photos.size() == 3);

    CHECK(c->albums().empty());
    const int64_t trip = c->create_album(" " + kTrip + " ");  // 前後の空白は落とす
    const int64_t best = c->create_album("Best");
    CHECK_THROWS_AS(c->create_album(kTrip), Error);  // 同じ名前
    CHECK_THROWS_AS(c->create_album("  "), Error);   // 空
    auto albums = c->albums();
    REQUIRE(albums.size() == 2);
    CHECK(albums[0].name == kTrip);  // 作った順
    CHECK(albums[1].name == "Best");

    const std::vector<int64_t> two = {photos[0].id, photos[1].id};
    c->add_to_album(trip, two);
    c->add_to_album(trip, two);  // 2 回足しても 1 回
    c->add_to_album(best, std::vector<int64_t>{photos[2].id});
    PhotoFilter f;
    f.album_id = trip;
    CHECK(c->count(f) == 2);
    CHECK(c->albums()[0].photo_count == 2);
    // ほかの条件と組み合わせる
    c->set_rating(std::vector<int64_t>{photos[0].id}, 4);
    f.min_rating = 3;
    CHECK(c->count(f) == 1);

    c->remove_from_album(trip, std::vector<int64_t>{photos[0].id});
    f.min_rating = 0;
    CHECK(c->count(f) == 1);

    c->rename_album(best, "Portfolio");
    CHECK_THROWS_AS(c->rename_album(best, kTrip), Error);
    CHECK(c->albums()[1].name == "Portfolio");

    // アルバムを消しても写真は消えない
    c->delete_album(trip);
    CHECK(c->albums().size() == 1);
    CHECK(all(*c).size() == 3);
    f.album_id = trip;
    CHECK(c->count(f) == 0);
}

TEST_CASE("カタログ: 最近の取り込みは、最後に写真を足した取り込みの写真", "[catalog][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Library lib;
    TempDir dir;
    auto c = Catalog::open(dir / "c.sqlite");
    PhotoFilter recent;
    recent.recent_import = true;
    CHECK(c->count(recent) == 0);  // まだ取り込んでいない

    const int64_t root = c->add_root(lib.root);
    c->scan_root(root);
    CHECK(c->count(recent) == 3);

    // 写真を足さなかった再スキャンでは変わらない
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));  // 時刻は秒単位
    c->scan_root(root);
    CHECK(c->count(recent) == 3);

    // 新しい写真を足すと、その回の写真だけ
    fs::copy_file(data_file("canon_eos_m50.CR3"), lib.root / "2019" / "NEW_0002.CR3");
    c->scan_root(root);
    const auto r = c->query(recent);
    REQUIRE(r.size() == 1);
    CHECK(r[0].file_name == "NEW_0002.CR3");
}
