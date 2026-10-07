// v3.19: SD カードなどからの取り込み（日付フォルダへのコピー・検証・重複のスキップ）
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>

#include "catalog/catalog.h"
#include "imaging/raw_decoder.h"
#include "edit/preset.h"
#include "import/card_import.h"
#include "test_util.h"
#include "thumbs/thumbnail.h"
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

TEST_CASE("カード取り込み: 一覧を撮影日時順に出し、選んだ写真だけ取り込む（v3.29）", "[import][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Card card;
    TempDir lib, db;
    auto c = Catalog::open(db / "c.sqlite");
    const fs::path dest = lib / "Photos";

    auto shots = list_card_shots(*c, card.dir.path(), dest);
    REQUIRE(shots.size() == 2);
    CHECK(shots[0].capture_time <= shots[1].capture_time);  // 撮影日時順
    for (const auto& s : shots) {
        CHECK_FALSE(s.imported);
        CHECK(s.is_photo);
        CHECK_FALSE(s.estimated);
        CHECK(s.has_raw);
    }
    const CardShot* pair = shots[0].name == "DSC00001.ARW" ? &shots[0] : &shots[1];
    const CardShot* single = pair == &shots[0] ? &shots[1] : &shots[0];
    CHECK(pair->files == 2);
    CHECK(pair->has_companion);
    CHECK(single->name == "DSC00002.CR3");
    CHECK_FALSE(single->has_companion);
    CHECK(fs::exists(pair->primary_path));

    // 1 枚だけ選んで取り込む。選ばなかった写真は skipped_unselected（取り込み済みとは別に数える）
    CardImportOptions opt;
    opt.source = card.dir.path();
    opt.dest_root = dest;
    opt.only = std::vector<std::string>{single->key};
    const CardImportResult r = import_from_card(*c, opt);
    CHECK(r.shots == 2);
    CHECK(r.imported == 1);
    CHECK(r.skipped_unselected == 1);
    CHECK(r.skipped_duplicates == 0);
    CHECK(r.photo_ids.size() == 1);
    const std::string day = day_of(data_file("sony_ilce7m3.ARW"));
    CHECK_FALSE(fs::exists(dest / day.substr(0, 4) / day / "DSC00001.ARW"));

    // 取り込んだ写真は「取り込み済み」になる。指定が空なら何も取り込まない
    shots = list_card_shots(*c, card.dir.path(), dest);
    REQUIRE(shots.size() == 2);
    for (const auto& s : shots) CHECK(s.imported == (s.name == "DSC00002.CR3"));
    opt.only = std::vector<std::string>{};
    const CardImportResult none = import_from_card(*c, opt);
    CHECK(none.imported == 0);
    CHECK(none.skipped_duplicates == 1);
    CHECK(none.skipped_unselected == 1);
    CHECK(none.photo_ids.empty());

    // 指定なしは、取り込み済み以外すべて（今までの動作）
    opt.only.reset();
    const CardImportResult rest = import_from_card(*c, opt);
    CHECK(rest.imported == 1);
    CHECK(fs::exists(dest / day.substr(0, 4) / day / "DSC00001.ARW"));
    CHECK(fs::exists(dest / day.substr(0, 4) / day / "DSC00001.JPG"));

    // キャンセルされた一覧は空
    std::atomic<bool> cancel{true};
    CHECK(list_card_shots(*c, card.dir.path(), dest, &cancel).empty());
}

TEST_CASE("カード取り込み: カード上のファイルのサムネイルをキャッシュに作る（v3.29）", "[import][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Card card;
    TempDir cache_dir;
    ThumbnailCache cache(cache_dir.path());
    const fs::path file = card.dcim / "DSC00002.CR3";
    const fs::path jpg = card_thumbnail(cache, file);
    CHECK(fs::exists(jpg));
    CHECK(jpg.extension() == ".jpg");
    const auto before = fs::last_write_time(jpg);
    CHECK(card_thumbnail(cache, file) == jpg);  // 2 回目はキャッシュから（作り直さない）
    CHECK((fs::last_write_time(jpg) == before));
    CHECK(card_thumbnail(cache, card.dcim / "DSC00001.ARW") != jpg);
    CHECK_THROWS_AS(card_thumbnail(cache, card.dcim / "missing.CR3"), Error);
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
    // file_clock::from_sys は古い libstdc++（Ubuntu 22.04 の GCC 11）にないので、いまとの差で作る
    const auto delta = std::chrono::system_clock::from_time_t(t) - std::chrono::system_clock::now();
    fs::last_write_time(jpg, fs::file_time_type::clock::now() +
                                 std::chrono::duration_cast<fs::file_time_type::duration>(delta));

    auto c = Catalog::open(db / "c.sqlite");
    CardImportOptions opt;
    opt.source = card.path();
    opt.dest_root = lib / "Photos";
    const CardImportResult r = import_from_card(*c, opt);
    CHECK(r.imported == 1);
    CHECK(r.estimated_dates == 1);
    CHECK(fs::exists(lib / "Photos" / "2020" / "2020-05-06" / "IMG_0001.JPG"));
    // JPEG だけの 1 枚も写真として登録する（v3.22）。中身が JPEG として読めないので「非対応」の行になる
    CHECK(c->count(PhotoFilter{}) == 1);

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

TEST_CASE("カード取り込み: 空き容量が足りなければ、コピーを始める前に止める", "[import]") {
    TempDir card, lib, db;
    write_file(card.path() / "DCIM" / "100A" / "IMG_0001.JPG", "some bytes");
    CHECK(free_space_bytes(lib.path()) > 0);
    CHECK(free_space_bytes(lib / "not" / "yet" / "created") > 0);  // 無い場所は近い親で数える

    auto c = Catalog::open(db / "c.sqlite");
    CardImportOptions opt;
    opt.source = card.path();
    opt.dest_root = lib / "Photos";
    opt.dry_run = true;
    const CardImportResult dry = import_from_card(*c, opt);
    CHECK(dry.bytes_needed == 10);
    CHECK(dry.space_available > 10);
}

TEST_CASE("カード取り込み: 登録はコピーしたファイルだけ。ルートの中のほかの写真には触れない", "[import][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Card card;
    TempDir lib, db;
    const fs::path root = lib / "Photos";
    fs::create_directories(root / "old");
    fs::copy_file(data_file("canon_eos_m50.CR3"), root / "old" / "OLD.CR3");
    fs::create_directories(root / "empty" / "deep");  // 写真のないフォルダ（整理の対象だが、取り込みでは触れない）
    auto c = Catalog::open(db / "c.sqlite");
    const int64_t root_id = c->add_root(root);
    c->scan_root(root_id);
    PhotoFilter all;
    const int64_t old_id = c->query(all)[0].id;
    c->set_rating(std::vector<int64_t>{old_id}, 4);

    // ルートの中で、OLD.CR3 を消し、別の RAW を足しておく（ルートを走査すれば、前者は「ファイルなし」、後者は新規になる）
    fs::remove(root / "old" / "OLD.CR3");
    fs::copy_file(data_file("canon_eos_m50.CR3"), root / "stray.CR3");

    CardImportOptions opt;
    opt.source = card.dir.path();
    opt.dest_root = root;
    const CardImportResult r = import_from_card(*c, opt);
    CHECK(r.imported == 2);
    CHECK(r.errors.empty());
    CHECK(r.scan.added == 2);
    CHECK(r.photo_ids.size() == 2);
    CHECK(r.scan.missing == 0);
    // 走査していないので、OLD は「ファイルなし」にならず、stray は登録されない
    CHECK(c->photo(old_id)->status == PhotoStatus::Ok);
    CHECK(c->photo(old_id)->rating == 4);
    CHECK(c->count(all) == 3);  // OLD + 取り込んだ 2 枚
    bool stray = false;
    for (const auto& p : c->query(all)) stray |= p.file_name == "stray.CR3";
    CHECK_FALSE(stray);
    CHECK(c->folder_id(root_id, "empty/deep").has_value());  // 空のフォルダの整理もしない

    // 取り込んだ日付フォルダは、親（年）とともに作られ、「最近の取り込み」は取り込んだ分だけになる
    const std::string day = day_of(data_file("sony_ilce7m3.ARW"));
    CHECK(c->folder_id(root_id, day.substr(0, 4)).has_value());
    CHECK(c->folder_id(root_id, day.substr(0, 4) + "/" + day).has_value());
    PhotoFilter recent;
    recent.recent_import = true;
    CHECK(c->count(recent) >= 2);

    // 登録のやり直し（同じファイルをもう一度）は unchanged
    std::vector<fs::path> again;
    for (const auto& p : c->query(all))
        if (p.file_name != "OLD.CR3") again.push_back(utf8_to_path(p.path));
    const ScanStats s2 = c->register_files(root_id, again);
    CHECK(s2.added == 0);
    CHECK(s2.unchanged == 2);
    // ルートの外・RAW でないものは Error
    CHECK_THROWS_AS(c->register_files(root_id, std::vector<fs::path>{card.dcim / "DSC00001.ARW"}), Error);
    CHECK_THROWS_AS(c->register_files(root_id, std::vector<fs::path>{card.dcim / "DSC00001.JPG"}), Error);
}

TEST_CASE("カード取り込み: 現像のプリセットを、取り込んだ写真だけに重ねる（v3.20）", "[import][data][preset]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Card card;
    TempDir lib, db;
    auto c = Catalog::open(db / "c.sqlite");
    CardImportOptions opt;
    opt.source = card.dir.path();
    opt.dest_root = lib / "Photos";
    Settings preset;
    preset.exposure = 0.5;
    preset.clarity = 20;
    preset.geometry.crop = {0, 0, 0.5, 0.5};  // 渡されても、プリセットの調整には含まれない（呼び出し側が落とす想定だが、重ねるのは調整だけ）
    opt.preset = preset_adjustments(preset);
    const CardImportResult r = import_from_card(*c, opt);
    REQUIRE(r.photo_ids.size() == 2);
    c->flush();
    for (int64_t id : r.photo_ids) {
        const auto json = c->edit_json(id);
        REQUIRE(json);
        const Settings s = settings_from_json(*json);
        CHECK(s.exposure == 0.5);
        CHECK(s.clarity == 20);
        CHECK(s.geometry == GeometrySettings{});
    }

    // 「なし」（プリセットなし）の取り込みでは、編集の行を作らない
    Card card2;
    TempDir lib2;
    auto c2 = Catalog::open(db / "c2.sqlite");
    CardImportOptions none;
    none.source = card2.dir.path();
    none.dest_root = lib2 / "Photos";
    const CardImportResult r2 = import_from_card(*c2, none);
    c2->flush();
    for (int64_t id : r2.photo_ids) CHECK_FALSE(c2->edit_json(id));
}
