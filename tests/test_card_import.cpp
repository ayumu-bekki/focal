// v3.19: SD カードなどからの取り込み（日付フォルダへのコピー・検証・重複のスキップ）
#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>

#include "catalog/catalog.h"
#include "imaging/raw_decoder.h"
#include "import/card_import.h"
#include "test_util.h"
#include "util/error.h"
#include "util/file.h"
#include "util/hash.h"

using namespace focal;
namespace fs = std::filesystem;

namespace {

fs::path data_file(const char* name) { return fs::path(FOCAL_TEST_DATA_DIR) / name; }

bool have_data() { return fs::exists(data_file("sony_ilce7m3.ARW")) && fs::exists(data_file("canon_eos_m50.CR3")); }

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}

std::string day_of(const fs::path& raw) {
    return capture_time_string(read_raw_metadata(raw).timestamp).value().substr(0, 10);
}

// カード: DCIM/100MSDCF/DSC00001.ARW + .JPG（RAW + JPEG のペア）、DSC00002.CR3、MOV と THM、サイドカーだけの孤児
struct Card {
    TempDir dir{"focal-card"};
    fs::path dcim = dir / "DCIM" / "100MSDCF";

    Card() {
        fs::create_directories(dcim);
        fs::copy_file(data_file("sony_ilce7m3.ARW"), dcim / "DSC00001.ARW");
        write_file(dcim / "DSC00001.JPG", "jpeg bytes of the pair");
        fs::copy_file(data_file("canon_eos_m50.CR3"), dcim / "DSC00002.CR3");
        write_file(dcim / "._DSC00001.ARW", "apple double");
        write_file(dcim / "ORPHAN.XMP", "sidecar only");
        write_file(dir / "DCIM" / "readme.txt", "x");
    }
};

} // namespace

TEST_CASE("カード取り込み: ペアを同じ日付フォルダへコピーし、カタログに登録する", "[import][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Card card;
    TempDir lib, db;
    auto c = Catalog::open(db / "c.sqlite");
    const fs::path dest = lib / "Photos";
    const int64_t album = c->create_album("Card");
    const int64_t tag = c->ensure_tag("Trip/Day1");

    const CardSummary sum = summarize_card(card.dir.path());  // ボリュームを渡す（DCIM を探す）
    CHECK(sum.shots == 2);
    CHECK(sum.files == 3);  // ペア 2 + CR3。孤児のサイドカーと ._ ファイルは数えない

    CardImportOptions opt;
    opt.source = card.dir.path();
    opt.dest_root = dest;
    opt.album_id = album;
    opt.tag_ids = {tag};
    std::vector<CardImportProgress::Phase> phases;
    opt.progress = [&](const CardImportProgress& p) {
        if (phases.empty() || phases.back() != p.phase) phases.push_back(p.phase);
    };
    const CardImportResult r = import_from_card(*c, opt);
    CHECK(r.shots == 2);
    CHECK(r.imported == 2);
    CHECK(r.failed == 0);
    CHECK(r.skipped_duplicates == 0);
    CHECK(r.estimated_dates == 0);
    CHECK(r.files_copied == 3);
    CHECK(r.photo_ids.size() == 2);
    CHECK(r.scan.added == 2);
    CHECK_FALSE(r.cancelled);
    REQUIRE(phases.size() == 3);
    CHECK(phases[0] == CardImportProgress::Phase::Reading);
    CHECK(phases[2] == CardImportProgress::Phase::Cataloging);

    // YYYY/YYYY-MM-DD/ に、同じ名前・同じ内容・同じ更新日時でコピーされる
    const std::string sony_day = day_of(data_file("sony_ilce7m3.ARW"));
    const fs::path sony_dir = dest / sony_day.substr(0, 4) / sony_day;
    REQUIRE(fs::exists(sony_dir / "DSC00001.ARW"));
    REQUIRE(fs::exists(sony_dir / "DSC00001.JPG"));  // ペアの JPEG も同じフォルダ
    CHECK(blake3_file_hex(sony_dir / "DSC00001.ARW") == blake3_file_hex(card.dcim / "DSC00001.ARW"));
    CHECK((fs::last_write_time(sony_dir / "DSC00001.ARW") == fs::last_write_time(card.dcim / "DSC00001.ARW")));
    const std::string canon_day = day_of(data_file("canon_eos_m50.CR3"));
    CHECK(fs::exists(dest / canon_day.substr(0, 4) / canon_day / "DSC00002.CR3"));
    // 途中の一時ファイルは残らない。カードには何も増えず、何も消えない
    for (const auto& e : fs::recursive_directory_iterator(dest))
        CHECK(e.path().filename().string().find(".focal-part") == std::string::npos);
    CHECK(fs::exists(card.dcim / "DSC00001.ARW"));
    CHECK(fs::exists(card.dcim / "DSC00001.JPG"));
    CHECK(fs::exists(card.dcim / "ORPHAN.XMP"));

    // 登録: コピー先がルートになり、JPEG は写真として登録されず、アルバムとタグが付く
    REQUIRE(c->roots().size() == 1);
    CHECK(r.root_id == c->roots()[0].id);
    CHECK(c->count(PhotoFilter{}) == 2);
    PhotoFilter f;
    f.album_id = album;
    CHECK(c->count(f) == 2);
    f = PhotoFilter{};
    f.tag_id = tag;
    CHECK(c->count(f) == 2);
    f = PhotoFilter{};
    f.recent_import = true;  // 「前回の読み込み」
    CHECK(c->count(f) == 2);

    // もう一度取り込むと、取り込み済みとしてスキップする
    CardImportOptions again = opt;
    again.progress = nullptr;
    const CardImportResult r2 = import_from_card(*c, again);
    CHECK(r2.skipped_duplicates == 2);
    CHECK(r2.imported == 0);
    CHECK(r2.files_copied == 0);
    CHECK(c->count(PhotoFilter{}) == 2);
}

TEST_CASE("カード取り込み: 取り込み済みの写真は、あとで別のフォルダへ移していてもスキップする", "[import][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Card card;
    TempDir lib, db;
    auto c = Catalog::open(db / "c.sqlite");
    CardImportOptions opt;
    opt.source = card.dir.path();
    opt.dest_root = lib / "Photos";
    REQUIRE(import_from_card(*c, opt).imported == 2);

    const std::string day = day_of(data_file("sony_ilce7m3.ARW"));
    fs::create_directories(lib / "Photos" / "Sorted");
    fs::rename(lib / "Photos" / day.substr(0, 4) / day / "DSC00001.ARW", lib / "Photos" / "Sorted" / "DSC00001.ARW");
    c->scan_root(c->roots()[0].id);
    const CardImportResult r = import_from_card(*c, opt);
    CHECK(r.skipped_duplicates >= 1);
    CHECK(fs::exists(lib / "Photos" / "Sorted" / "DSC00001.ARW"));
    CHECK_FALSE(fs::exists(lib / "Photos" / day.substr(0, 4) / day / "DSC00001.ARW"));  // 戻ってこない
}

TEST_CASE("カード取り込み: 同名の別ファイルは上書きせず連番を付け、ペアの幹をそろえる", "[import][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Card card;
    TempDir lib, db;
    auto c = Catalog::open(db / "c.sqlite");
    const std::string day = day_of(data_file("sony_ilce7m3.ARW"));
    const fs::path dir = lib / "Photos" / day.substr(0, 4) / day;
    write_file(dir / "DSC00001.ARW", "another camera's file with the same name");

    CardImportOptions opt;
    opt.source = card.dir.path();
    opt.dest_root = lib / "Photos";
    const CardImportResult r = import_from_card(*c, opt);
    CHECK(r.imported == 2);
    CHECK(r.failed == 0);
    // 元のファイルはそのまま。新しい方は DSC00001_1.ARW / DSC00001_1.JPG
    std::ifstream in(dir / "DSC00001.ARW", std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    CHECK(ss.str() == "another camera's file with the same name");
    CHECK(fs::exists(dir / "DSC00001_1.ARW"));
    CHECK(fs::exists(dir / "DSC00001_1.JPG"));
    CHECK(blake3_file_hex(dir / "DSC00001_1.ARW") == blake3_file_hex(card.dcim / "DSC00001.ARW"));

    // 続けてもう一度: 連番の方に同じファイルがあるので取り込み済み
    CHECK(import_from_card(*c, opt).imported == 0);
}

TEST_CASE("カード取り込み: 撮影日時が読めないものはファイルの更新日時で日付を決め、推定と数える", "[import]") {
    TempDir card, lib, db;
    const fs::path jpg = card.path() / "DCIM" / "100CANON" / "IMG_0001.JPG";
    write_file(jpg, "just a jpeg");
    // 2020-05-06 12:00（ローカル時刻）
    std::tm tm{};
    tm.tm_year = 120;
    tm.tm_mon = 4;
    tm.tm_mday = 6;
    tm.tm_hour = 12;
    tm.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm);
    fs::last_write_time(jpg, fs::file_time_type::clock::from_sys(std::chrono::system_clock::from_time_t(t)));

    auto c = Catalog::open(db / "c.sqlite");
    CardImportOptions opt;
    opt.source = card.path();
    opt.dest_root = lib / "Photos";
    const CardImportResult r = import_from_card(*c, opt);
    CHECK(r.imported == 1);
    CHECK(r.estimated_dates == 1);
    CHECK(fs::exists(lib / "Photos" / "2020" / "2020-05-06" / "IMG_0001.JPG"));
    CHECK(c->count(PhotoFilter{}) == 0);  // JPEG は写真として登録しない（RAW だけ）

    // 2 回目: コピー先に同じ名前・大きさ・更新日時があるのでスキップ
    CHECK(import_from_card(*c, opt).skipped_duplicates == 1);
}

TEST_CASE("カード取り込み: ドライラン・キャンセル・コピーの検証", "[import]") {
    TempDir card, lib, db;
    write_file(card.path() / "DCIM" / "100A" / "IMG_0001.JPG", "one");
    write_file(card.path() / "DCIM" / "100A" / "IMG_0002.JPG", "two");
    auto c = Catalog::open(db / "c.sqlite");

    CardImportOptions opt;
    opt.source = card.path() / "DCIM";  // DCIM フォルダそのものも指定できる
    opt.dest_root = lib / "Photos";
    opt.dry_run = true;
    const CardImportResult dry = import_from_card(*c, opt);
    CHECK(dry.imported == 2);
    CHECK(dry.files_copied == 0);
    CHECK_FALSE(fs::exists(lib / "Photos"));  // 何も作らない
    CHECK(c->roots().empty());

    std::atomic<bool> cancel{true};
    opt.dry_run = false;
    opt.cancel = &cancel;
    const CardImportResult cancelled = import_from_card(*c, opt);
    CHECK(cancelled.cancelled);
    CHECK(cancelled.files_copied == 0);

    // コピーの部品: 一時ファイル → 検証 → 名前の変更。既存の宛先は上書きしない
    const fs::path src = card.path() / "DCIM" / "100A" / "IMG_0001.JPG";
    const fs::path out = lib / "x" / "copy.JPG";
    fs::create_directories(out.parent_path());
    int64_t bytes = 0;
    copy_file_verified(src, out, true, nullptr, &bytes);
    CHECK(bytes == 3);
    CHECK(blake3_file_hex(out) == blake3_file_hex(src));
    CHECK_THROWS_AS(copy_file_verified(src, out, true), Error);
    CHECK(blake3_file_hex(out) == blake3_file_hex(src));
    CHECK_THROWS_AS(copy_file_verified(card.path() / "nothing.JPG", lib / "x" / "n.JPG", true), Error);
    for (const auto& e : fs::directory_iterator(lib / "x")) CHECK(e.path().filename() == "copy.JPG");  // 一時ファイルなし

    // コピー先が作れなければ Error
    write_file(lib / "file", "x");
    CardImportOptions bad = opt;
    bad.cancel = nullptr;
    bad.dest_root = lib / "file" / "sub";
    CHECK_THROWS_AS(import_from_card(*c, bad), Error);
}

TEST_CASE("カード取り込み: 中身がない・存在しないソース", "[import]") {
    TempDir card, lib, db;
    auto c = Catalog::open(db / "c.sqlite");
    CardImportOptions opt;
    opt.source = card.path();
    opt.dest_root = lib / "Photos";
    const CardImportResult r = import_from_card(*c, opt);
    CHECK(r.shots == 0);
    CHECK(r.imported == 0);
    CHECK(c->roots().empty());
    opt.source = card.path() / "missing";
    CHECK_THROWS_AS(import_from_card(*c, opt), Error);
    CHECK_THROWS_AS(summarize_card(card.path() / "missing"), Error);
}
