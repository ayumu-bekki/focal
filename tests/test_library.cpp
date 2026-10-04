// v3.19: アルバムのフォルダ分け・スマートアルバム・ボリューム・移動した写真のつなぎ直し
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>

#include "catalog/catalog.h"
#include "catalog/smart_query.h"
#include "catalog/sqlite.h"
#include "test_util.h"
#include "util/error.h"
#include "util/file.h"
#include "util/volume.h"

using namespace focal;
namespace fs = std::filesystem;

namespace {

fs::path data_file(const char* name) { return fs::path(FOCAL_TEST_DATA_DIR) / name; }

bool have_data() { return fs::exists(data_file("sony_ilce7m3.ARW")) && fs::exists(data_file("canon_eos_m50.CR3")); }

// root/2018/A.ARW（Sony）、root/2018/B.CR3（Canon）、root/2019/C.CR3（Canon）
struct Lib {
    TempDir dir{"focal-lib3"};
    fs::path root = dir / "photos";

    Lib() {
        fs::create_directories(root / "2018");
        fs::create_directories(root / "2019");
        fs::copy_file(data_file("sony_ilce7m3.ARW"), root / "2018" / "A.ARW");
        fs::copy_file(data_file("canon_eos_m50.CR3"), root / "2018" / "B.CR3");
        fs::copy_file(data_file("canon_eos_m50.CR3"), root / "2019" / "C.CR3");
    }
};

int64_t id_of(Catalog& c, const std::string& name) {
    for (const auto& p : c.query(PhotoFilter{}))
        if (p.file_name == name) return p.id;
    FAIL("no photo " << name);
    return 0;
}

int64_t count_smart(Catalog& c, int64_t album) {
    PhotoFilter f;
    f.smart_album_id = album;
    return c.count(f);
}

} // namespace

TEST_CASE("アルバムのフォルダ: 入れ子・親ごとの名前の一意・移動・削除", "[library]") {
    TempDir dir;
    auto c = Catalog::open(dir / "c.sqlite");
    const int64_t trip = c->create_album_folder("Trip");
    const int64_t hokkaido = c->create_album("Hokkaido", trip);
    const int64_t best = c->create_album("Best");
    // 別のフォルダの下なら同じ名前を使える（v2 までは全体で一意だった）
    const int64_t work = c->create_album_folder("Work");
    const int64_t hok2 = c->create_album("Hokkaido", work);
    CHECK_THROWS_AS(c->create_album("Hokkaido", trip), Error);  // 同じ親の下では重複できない
    CHECK_THROWS_AS(c->create_album("Best"), Error);            // ルートでも同じ
    CHECK_THROWS_AS(c->create_album("X", best), Error);         // 親になれるのはフォルダだけ
    CHECK_THROWS_AS(c->create_album("X", 9999), Error);

    // 親が先、同じ親の中は作った順
    const auto list = c->albums();
    REQUIRE(list.size() == 5);
    CHECK(list[0].id == trip);
    CHECK(list[0].kind == AlbumKind::Folder);
    CHECK(list[1].id == hokkaido);
    CHECK(list[1].parent_id == trip);
    CHECK(list[2].id == best);
    CHECK(list[3].id == work);
    CHECK(list[4].id == hok2);

    c->rename_album(hok2, "Tokyo");
    CHECK_NOTHROW(c->rename_album(hokkaido, "Hokkaido"));  // 自分の名前のままなら重複ではない
    c->create_album("Tokyo", trip);
    CHECK_THROWS_AS(c->rename_album(hokkaido, "Tokyo"), Error);  // 同じ親の下に同じ名前
}

TEST_CASE("アルバムのフォルダ: 移動は自分の中へは行けず、消すと中身も消える", "[library]") {
    TempDir dir;
    auto c = Catalog::open(dir / "c.sqlite");
    const int64_t a = c->create_album_folder("A");
    const int64_t b = c->create_album_folder("B", a);
    const int64_t x = c->create_album("X", b);
    CHECK_THROWS_AS(c->move_album(a, b), Error);  // 子孫の中へ
    CHECK_THROWS_AS(c->move_album(a, a), Error);
    c->move_album(x, std::nullopt);
    CHECK_FALSE(c->albums().back().parent_id.has_value());
    c->move_album(x, a);
    CHECK(c->albums()[1].id == b);  // a の下に b と x
    const int64_t clash = c->create_album("X");
    CHECK_THROWS_AS(c->move_album(clash, a), Error);  // 移動先に同じ名前

    c->delete_album(a);  // フォルダを消すと中のアルバムも消える
    const auto left = c->albums();
    REQUIRE(left.size() == 1);
    CHECK(left[0].id == clash);
}

TEST_CASE("アルバムのフォルダ: 写真を足せるのは手で集めるアルバムだけ", "[library]") {
    TempDir dir;
    auto c = Catalog::open(dir / "c.sqlite");
    const int64_t folder = c->create_album_folder("F");
    const int64_t smart = c->create_smart_album("S", empty_smart_query());
    const std::vector<int64_t> ids = {1};
    CHECK_THROWS_AS(c->add_to_album(folder, ids), Error);
    CHECK_THROWS_AS(c->add_to_album(smart, ids), Error);
    CHECK_THROWS_AS(c->add_to_album(777, ids), Error);
    CHECK_THROWS_AS(c->set_smart_query(folder, empty_smart_query()), Error);
}

TEST_CASE("スマートアルバム: 条件の検証", "[library]") {
    CHECK_NOTHROW(validate_smart_query(empty_smart_query()));
    CHECK_NOTHROW(validate_smart_query(R"({"match":"any","rules":[
        {"field":"rating","op":">=","value":4},
        {"field":"flag","op":"is_not","value":"reject"},
        {"field":"tag","op":"not_has","value":3},
        {"field":"camera","op":"contains","value":"Canon"},
        {"field":"iso","op":"<=","value":1600},
        {"field":"f_number","op":">=","value":1.8},
        {"field":"date","op":"between","value":["2020-01-01","2020-12-31"]}]})"));
    for (const char* bad : {
             "not json",
             "[]",
             R"({"match":"some","rules":[]})",
             R"({"rules":[{"field":"nope","op":"=","value":1}]})",
             R"({"rules":[{"field":"rating","op":"~","value":1}]})",
             R"({"rules":[{"field":"rating","op":">=","value":9}]})",
             R"({"rules":[{"field":"rating","op":">=","value":"4"}]})",
             R"({"rules":[{"field":"flag","op":"is","value":"star"}]})",
             R"({"rules":[{"field":"tag","op":"has","value":"風景"}]})",
             R"({"rules":[{"field":"date","op":"between","value":["2020-01-01"]}]})",
             R"({"rules":[{"field":"date","op":">=","value":"2020/01/01"}]})",
             R"({"rules":[{"field":"camera","op":"contains","value":""}]})",
             R"({"rules":[{"field":"rating","op":">="}]})",
             R"({"rules":[{"field":3,"op":">=","value":1}]})",
         })
        CHECK_THROWS_AS(validate_smart_query(bad), Error);
    // 壊れた条件を保存済みでも、問い合わせは何にも一致しないだけで落ちない
    CHECK(smart_query_clause("not json").sql == "0 = 1");
}

TEST_CASE("スマートアルバム: AND・OR・NOT とアルバム・タグ・EXIF の条件", "[library][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Lib lib;
    TempDir dir;
    auto c = Catalog::open(dir / "c.sqlite");
    c->scan_root(c->add_root(lib.root));
    const int64_t a = id_of(*c, "A.ARW"), b = id_of(*c, "B.CR3"), cc = id_of(*c, "C.CR3");
    c->set_rating(std::vector<int64_t>{a}, 5);
    c->set_rating(std::vector<int64_t>{b}, 3);
    c->set_flag(std::vector<int64_t>{cc}, -1);
    const int64_t tag = c->ensure_tag("Nature/Mountain");
    c->add_tag(std::vector<int64_t>{b, cc}, tag);
    const int64_t album = c->create_album("Pick");
    c->add_to_album(album, std::vector<int64_t>{a, b});

    auto smart = [&](const std::string& json) { return count_smart(*c, c->create_smart_album("s" + json, json)); };

    CHECK(smart(empty_smart_query()) == 3);
    CHECK(smart(R"({"rules":[{"field":"rating","op":">=","value":4}]})") == 1);
    // タグが付いていて、リジェクトではない（NOT 条件）
    CHECK(smart(R"({"match":"all","rules":[
        {"field":"tag","op":"has","value":)" + std::to_string(c->find_tag("Nature").value()) + R"(},
        {"field":"flag","op":"is_not","value":"reject"}]})") == 1);
    // OR: ★4 以上、またはリジェクト
    CHECK(smart(R"({"match":"any","rules":[
        {"field":"rating","op":">=","value":4},{"field":"flag","op":"is","value":"reject"}]})") == 2);
    // アルバムに含まれる中で ★3 以上、アルバムに含まれない
    CHECK(smart(R"({"rules":[{"field":"album","op":"in","value":)" + std::to_string(album) +
                R"(},{"field":"rating","op":">=","value":4}]})") == 1);
    CHECK(smart(R"({"rules":[{"field":"album","op":"not_in","value":)" + std::to_string(album) + R"(}]})") == 1);
    // EXIF
    CHECK(smart(R"({"rules":[{"field":"camera","op":"contains","value":"canon"}]})") == 2);
    CHECK(smart(R"({"rules":[{"field":"camera","op":"not_contains","value":"canon"}]})") == 1);
    CHECK(smart(R"({"rules":[{"field":"file_name","op":"contains","value":"%"}]})") == 0);  // LIKE の記号は文字として扱う
    // フォルダ
    CHECK(smart(R"({"rules":[{"field":"folder","op":"in","value":)" +
                std::to_string(c->folder_id(c->roots()[0].id, "2019").value()) + R"(}]})") == 1);

    // 一覧の枚数と、絞り込みとの組み合わせ
    const int64_t hi = c->create_smart_album("Hi", R"({"rules":[{"field":"rating","op":">=","value":3}]})");
    bool found = false;
    for (const auto& al : c->albums())
        if (al.id == hi) {
            found = true;
            CHECK(al.kind == AlbumKind::Smart);
            CHECK(al.photo_count == 2);
        }
    CHECK(found);
    PhotoFilter f;
    f.smart_album_id = hi;
    f.min_rating = 4;
    CHECK(c->count(f) == 1);
    CHECK(c->query(f)[0].file_name == "A.ARW");
    CHECK(c->smart_query(hi).has_value());
    CHECK_FALSE(c->smart_query(album).has_value());

    // 条件を変えると結果も変わる。スマートアルバム自体は写真を持たない
    c->set_smart_query(hi, R"({"rules":[{"field":"rating","op":">=","value":5}]})");
    CHECK(count_smart(*c, hi) == 1);
    CHECK_THROWS_AS(c->set_smart_query(hi, "{"), Error);
    CHECK(count_smart(*c, hi) == 1);  // 不正な条件では保存されない
    CHECK(count_smart(*c, 424242) == 0);
}

TEST_CASE("ボリューム: ルートにボリュームの ID と相対パスを記録し、マウントポイントの変化に追従する", "[library][volume]") {
    TempDir dir;
    const fs::path root = dir / "a" / "photos";
    fs::create_directories(root);
    const auto vol = volume_for_path(root);
    if (!vol || vol->id.empty()) SKIP("この環境ではボリュームの ID を取得できない");
    const auto rel = volume_relative_path(*vol, root);
    REQUIRE(rel.has_value());
    CHECK_FALSE(rel->empty());
    CHECK(fs::weakly_canonical(vol->mount_point / utf8_to_path(*rel)) == fs::weakly_canonical(root));

    const fs::path db_path = dir / "c.sqlite";
    auto c = Catalog::open(db_path);
    c->add_root(root);
    auto roots = c->roots();
    REQUIRE(roots.size() == 1);
    CHECK(roots[0].volume_id == vol->id);
    CHECK(roots[0].volume_rel_path == *rel);
    CHECK(roots[0].online);

    // ドライブレター・マウントポイントが変わった状態を作る: 保存してあるパスだけ別の場所にする
    {
        db::Database db(db_path, db::Database::Mode::ReadWrite);
        db.exec("UPDATE roots SET path = '/definitely/not/here/photos'");
    }
    CHECK_FALSE(c->roots()[0].online);
    CHECK(c->refresh_volumes() == 1);  // ボリューム ID から今の場所を求めて戻す
    roots = c->roots();
    CHECK(roots[0].online);
    CHECK(roots[0].path == normalized_path_string(vol->mount_point / utf8_to_path(*rel)));
    CHECK(c->refresh_volumes() == 0);

    // 外れているボリューム（ID が見つからない）は何も変えない
    {
        db::Database db(db_path, db::Database::Mode::ReadWrite);
        db.exec("UPDATE roots SET volume_id = 'NO-SUCH-VOLUME', path = '/definitely/not/here/photos'");
    }
    CHECK(c->refresh_volumes() == 0);
    roots = c->roots();
    CHECK_FALSE(roots[0].online);
    CHECK(roots[0].path == "/definitely/not/here/photos");
    CHECK_THROWS_AS(c->scan_root(roots[0].id), Error);  // 写真をファイルなしにはしない
}

TEST_CASE("ルート: 含むルートを探し、ボリューム情報のない古いルートは起動時に補う", "[library][volume]") {
    TempDir dir;
    const fs::path outer = dir / "outer";
    fs::create_directories(outer / "inner" / "deep");
    const fs::path db_path = dir / "c.sqlite";
    auto c = Catalog::open(db_path);
    const int64_t o = c->add_root(outer);
    const int64_t i = c->add_root(outer / "inner");
    CHECK(c->root_containing(outer / "inner" / "deep")->id == i);  // いちばん深いルート
    CHECK(c->root_containing(outer / "other")->id == o);
    CHECK(c->root_containing(dir / "elsewhere") == std::nullopt);
    CHECK(c->root_containing(outer)->id == o);

    c->set_root_label(o, "Main");
    CHECK(c->root_for_path(outer)->label == "Main");

    const auto vol = volume_for_path(outer);
    if (!vol || vol->id.empty()) SKIP("この環境ではボリュームの ID を取得できない");
    {
        db::Database db(db_path, db::Database::Mode::ReadWrite);
        db.exec("UPDATE roots SET volume_id = NULL, volume_name = NULL, volume_rel_path = NULL");
    }
    CHECK(c->roots()[0].volume_id.empty());
    c->refresh_volumes();
    for (const auto& r : c->roots()) CHECK(r.volume_id == vol->id);
}

TEST_CASE("再スキャン: 別のフォルダへ移した写真は、★・タグ・アルバムを引き継いでつなぎ直す", "[library][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Lib lib;
    TempDir dir;
    auto c = Catalog::open(dir / "c.sqlite");
    const int64_t root = c->add_root(lib.root);
    c->scan_root(root);
    const int64_t b = id_of(*c, "B.CR3");
    c->set_rating(std::vector<int64_t>{b}, 4);
    const int64_t album = c->create_album("Keep");
    c->add_to_album(album, std::vector<int64_t>{b});
    c->add_tag(std::vector<int64_t>{b}, c->ensure_tag("t"));

    fs::create_directories(lib.root / "moved");
    fs::rename(lib.root / "2018" / "B.CR3", lib.root / "moved" / "B.CR3");
    const ScanStats s = c->scan_root(root);
    CHECK(s.relinked == 1);
    CHECK(s.added == 0);
    CHECK(s.missing == 1);  // 移動の前に「ファイルなし」と数えてから戻す（v1 と同じ数え方）
    const auto p = c->photo(b);
    REQUIRE(p.has_value());
    CHECK(p->status == PhotoStatus::Ok);
    CHECK(p->rating == 4);
    CHECK(p->path.find("/moved/") != std::string::npos);
    CHECK(c->photo_tags(b).size() == 1);
    PhotoFilter f;
    f.album_id = album;
    CHECK(c->count(f) == 1);
    CHECK(c->count(PhotoFilter{}) == 3);  // 重複して増えない

    // ファイルなしの候補が 2 枚ありどちらか決められないときは、新規として足す
    fs::copy_file(data_file("canon_eos_m50.CR3"), lib.root / "2018" / "X.CR3");
    fs::copy_file(data_file("canon_eos_m50.CR3"), lib.root / "2019" / "X.CR3");
    c->scan_root(root);
    fs::remove(lib.root / "2018" / "X.CR3");
    fs::remove(lib.root / "2019" / "X.CR3");
    fs::copy_file(data_file("canon_eos_m50.CR3"), lib.root / "moved" / "X.CR3");
    const ScanStats s2 = c->scan_root(root);
    CHECK(s2.relinked == 0);
    CHECK(s2.added == 1);
    CHECK(s2.missing == 2);
}

TEST_CASE("ルートを外すと写真の情報は消え、ファイルは残る", "[library][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Lib lib;
    TempDir dir;
    auto c = Catalog::open(dir / "c.sqlite");
    const int64_t root = c->add_root(lib.root);
    c->scan_root(root);
    const int64_t b = id_of(*c, "B.CR3");
    const int64_t album = c->create_album("Keep");
    c->add_to_album(album, std::vector<int64_t>{b});
    c->add_tag(std::vector<int64_t>{b}, c->ensure_tag("t"));
    CHECK_THROWS_AS(c->remove_root(9999), Error);

    c->remove_root(root);
    CHECK(c->roots().empty());
    CHECK(c->count(PhotoFilter{}) == 0);
    CHECK(c->albums().size() == 1);  // アルバム自体は残る（空になる）
    CHECK(c->albums()[0].photo_count == 0);
    CHECK(fs::exists(lib.root / "2018" / "B.CR3"));  // ファイルは消さない
    // もう一度追加できる
    c->scan_root(c->add_root(lib.root));
    CHECK(c->count(PhotoFilter{}) == 3);
}

TEST_CASE("ネットワークの共有は、ユーザー名やマウントポイントが変わっても同じボリューム ID になる", "[library][volume]") {
    CHECK(network_volume_id("//guest@NAS.local/Photos") == "net:nas.local/photos");
    CHECK(network_volume_id("//other:pw@nas.local/Photos/") == "net:nas.local/photos");
    CHECK(network_volume_id("//nas.local/Photos") == "net:nas.local/photos");
    CHECK(network_volume_id("nas:/export/Photos") == "net:nas:/export/photos");
    CHECK(network_volume_id("/dev/disk3s1").empty());  // ローカルのデバイス
    CHECK(network_volume_id("C:\\").empty());          // ドライブレター
    CHECK(network_volume_id("tmpfs").empty());
    CHECK(network_volume_id("").empty());
}

TEST_CASE("到達できるかの確認: あるフォルダは true、ないフォルダは false で、待たされない", "[library][volume]") {
    TempDir dir;
    fs::create_directories(dir / "a");
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = directories_reachable({normalized_path_string(dir / "a"), normalized_path_string(dir / "missing"),
                                          normalized_path_string(dir / "a" / "nothing")},
                                         std::chrono::milliseconds(1500));
    REQUIRE(r.size() == 3);
    CHECK(r[0]);
    CHECK_FALSE(r[1]);
    CHECK_FALSE(r[2]);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1400));
    CHECK(directories_reachable({}, std::chrono::milliseconds(10)).empty());
}

TEST_CASE("アルバムの並べ替えとカバー写真", "[library][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Lib lib;
    TempDir dir;
    auto c = Catalog::open(dir / "c.sqlite");
    c->scan_root(c->add_root(lib.root));
    const int64_t a = c->create_album("A"), b = c->create_album("B"), cc = c->create_album("C");
    const int64_t folder = c->create_album_folder("F");
    const int64_t inner = c->create_album("Inner", folder);
    auto names = [&] {
        std::string s;
        for (const auto& al : c->albums()) s += al.name + ",";
        return s;
    };
    CHECK(names() == "A,B,C,F,Inner,");
    c->move_album_order(c->albums()[1].id, -1);  // B を上へ
    CHECK(names() == "B,A,C,F,Inner,");
    c->move_album_order(a, 1);  // A を下へ
    CHECK(names() == "B,C,A,F,Inner,");
    c->move_album_order(b, -1);  // 端は何もしない
    c->move_album_order(folder, 1);  // 端
    CHECK(names() == "B,C,A,F,Inner,");
    c->move_album_order(inner, 1);  // 兄弟がいない
    CHECK(names() == "B,C,A,F,Inner,");
    c->move_album_order(folder, -1);
    CHECK(names() == "B,C,F,Inner,A,");
    CHECK_THROWS_AS(c->move_album_order(9999, 1), Error);
    (void)cc;

    // カバー: 指定がなければ先頭の写真（撮影日時順）。そのアルバムの写真しか指定できない
    const int64_t p1 = id_of(*c, "A.ARW"), p2 = id_of(*c, "B.CR3");
    c->add_to_album(a, std::vector<int64_t>{p1, p2});
    auto cover = [&](int64_t id) {
        for (const auto& al : c->albums())
            if (al.id == id) return al.cover_photo_id;
        return std::optional<int64_t>();
    };
    CHECK(cover(a) == p1);  // 2018-03-13 の Sony が先
    CHECK_FALSE(cover(b).has_value());  // 空
    c->set_album_cover(a, p2);
    CHECK(cover(a) == p2);
    CHECK_THROWS_AS(c->set_album_cover(b, p1), Error);  // B の写真ではない
    c->set_album_cover(a, std::nullopt);
    CHECK(cover(a) == p1);
}

TEST_CASE("スキャン: 「写真」などのライブラリ（.photoslibrary ほか）の中へは入らない", "[library][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Lib lib;
    for (const char* pkg : {"Photos Library.photoslibrary", "Old.aplibrary", "Previews.lrdata", "UPPER.PHOTOSLIBRARY"}) {
        fs::create_directories(lib.root / pkg / "originals" / "0");
        fs::copy_file(data_file("canon_eos_m50.CR3"), lib.root / pkg / "originals" / "0" / "X.CR3");
    }
    fs::create_directories(lib.root / "Trip.photos" / "sub");  // 拡張子が似ていても別物は入る
    fs::copy_file(data_file("canon_eos_m50.CR3"), lib.root / "Trip.photos" / "sub" / "D.CR3");

    TempDir dir;
    auto c = Catalog::open(dir / "c.sqlite");
    const int64_t root = c->add_root(lib.root);
    c->scan_root(root);
    std::vector<std::string> names;
    for (const auto& p : c->query(PhotoFilter{})) names.push_back(p.file_name);
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string>{"A.ARW", "B.CR3", "C.CR3", "D.CR3"});  // X.CR3 は入らない
    for (const auto& f : c->folders(root)) CHECK(f.rel_path.find("library") == std::string::npos);
}
