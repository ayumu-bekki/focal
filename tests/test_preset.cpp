// v3.20: 現像のプリセット（調整だけを保存し、切り取りなどは含めない）
#include <catch2/catch_test_macros.hpp>

#include <fstream>

#include "catalog/catalog.h"
#include "edit/preset.h"
#include "import/card_import.h"
#include "test_util.h"
#include "util/error.h"

using namespace focal;
namespace fs = std::filesystem;

namespace {

Settings adjusted() {
    Settings s;
    s.exposure = 0.7;
    s.contrast = 15;
    s.vibrance = 20;
    s.clarity = 30;
    s.sharpness = 40;
    s.noise_reduction = 25;
    s.wb.mode = WhiteBalanceSettings::Mode::Custom;
    s.wb.temperature = 5200;
    s.wb.tint = 4;
    s.geometry.rotate90 = 1;
    s.geometry.straighten = 2.5;
    s.geometry.crop = {0.1, 0.1, 0.5, 0.5};
    s.geometry.aspect = AspectMode::R3x2;
    return s;
}

} // namespace

TEST_CASE("プリセット: 調整だけを持ち、切り取り・回転・傾きは含めない", "[preset]") {
    const Settings p = preset_adjustments(adjusted());
    CHECK(p.geometry == GeometrySettings{});
    CHECK(p.exposure == 0.7);
    CHECK(p.wb.mode == WhiteBalanceSettings::Mode::Custom);

    // 重ねると、調整はプリセットのもの、切り取りなどは写真のまま
    Settings photo;
    photo.exposure = -1;
    photo.saturation = 50;  // プリセットは 0 → 0 に戻る（調整は丸ごとプリセットの値）
    photo.geometry.rotate90 = 3;
    photo.geometry.crop = {0, 0, 0.8, 0.8};
    const Settings r = apply_preset(photo, p);
    CHECK(r.exposure == 0.7);
    CHECK(r.saturation == 0.0);
    CHECK(r.clarity == 30);
    CHECK(r.geometry == photo.geometry);
}

TEST_CASE("プリセット: 保存・一覧・読み込み・上書き・削除", "[preset]") {
    TempDir user, builtin;
    PresetStore store(user.path(), builtin.path());
    CHECK(store.list().empty());

    const std::string id = store.save("日本語/風景: 標準", adjusted());
    CHECK(id.rfind("user:", 0) == 0);
    REQUIRE(store.list().size() == 1);
    CHECK(store.list()[0].name == "日本語/風景: 標準");
    CHECK_FALSE(store.list()[0].builtin);

    const Settings loaded = store.load(id);
    CHECK(loaded.geometry == GeometrySettings{});  // 切り取りは保存されない
    CHECK(loaded.exposure == 0.7);
    CHECK(loaded.contrast == 15);
    CHECK(loaded.sharpness == 40);
    CHECK(loaded.wb.temperature == 5200);

    // 同じ名前（大文字小文字は区別しない）は上書き
    Settings other;
    other.exposure = -0.5;
    CHECK(store.save("日本語/風景: 標準", other) == id);
    CHECK(store.list().size() == 1);
    CHECK(store.load(id).exposure == -0.5);
    store.save("a", other);
    CHECK(store.save("A", adjusted()) == store.save("a", adjusted()));
    CHECK(store.list().size() == 2);

    // ファイル名が衝突する別の名前は連番にする
    const std::string x = store.save("x?", other), y = store.save("x*", other);
    CHECK(x != y);

    CHECK_THROWS_AS(store.save("   ", other), Error);
    CHECK_THROWS_AS(store.load("user:../etc"), Error);
    CHECK_THROWS_AS(store.load("user:none"), Error);
    store.remove(id);
    CHECK_THROWS_AS(store.load(id), Error);
    CHECK_THROWS_AS(store.remove("builtin:x"), Error);
}

TEST_CASE("プリセット: 同梱のものは先に並び、消せない。壊れたファイルは飛ばす", "[preset]") {
    TempDir user, builtin;
    {
        PresetStore maker(builtin.path());
        Settings s;
        s.exposure = 1;
        maker.save("Focal Standard", s);
    }
    std::ofstream(user.path() / "broken.focalpreset") << "{not json";
    std::ofstream(user.path() / "other.json") << "{}";
    PresetStore store(user.path(), builtin.path());
    store.save("Mine", Settings{});
    const auto list = store.list();
    REQUIRE(list.size() == 2);
    CHECK(list[0].builtin);
    CHECK(list[0].name == "Focal Standard");
    CHECK(list[1].name == "Mine");
    CHECK(store.load(list[0].id).exposure == 1);
    CHECK_THROWS_AS(store.remove(list[0].id), Error);
}

TEST_CASE("プリセット: カタログの写真に重ねる（切り取りは残し、既定値だけなら行を消す）", "[preset][catalog]") {
    TempDir lib, db;
    fs::create_directories(lib.path() / "d");
    // 中身のないファイルでも、写真の行は作られる（「非対応」扱い）
    std::ofstream(lib.path() / "d" / "a.NEF") << "x";
    std::ofstream(lib.path() / "d" / "b.NEF") << "yy";
    auto c = Catalog::open(db / "c.sqlite");
    const int64_t root = c->add_root(lib.path());
    c->scan_root(root);
    const auto ids = c->query_ids(PhotoFilter{});
    REQUIRE(ids.size() == 2);

    // a は切り取り済み
    Settings cropped;
    cropped.geometry.crop = {0, 0, 0.5, 0.5};
    cropped.exposure = 2;
    c->save_edit(ids[0], 1, settings_to_json(cropped));
    c->flush();

    Settings preset;
    preset.contrast = 10;
    c->apply_preset(ids, preset);
    c->flush();
    const Settings a = settings_from_json(*c->edit_json(ids[0]));
    CHECK(a.contrast == 10);
    CHECK(a.exposure == 0.0);  // 調整はプリセットの値になる
    CHECK(a.geometry.crop == CropRect{0, 0, 0.5, 0.5});
    const Settings b = settings_from_json(*c->edit_json(ids[1]));
    CHECK(b.contrast == 10);

    // 「なし」のプリセット（すべて既定値）を重ねると、切り取りのない写真は編集の行がなくなる。切り取りのある写真は残る
    c->apply_preset(ids, Settings{});
    c->flush();
    CHECK_FALSE(c->edit_json(ids[1]));
    REQUIRE(c->edit_json(ids[0]));
    CHECK(settings_from_json(*c->edit_json(ids[0])).geometry.crop == CropRect{0, 0, 0.5, 0.5});
}

TEST_CASE("プリセット: 調整が同じプリセットを探す・名前を変える", "[preset]") {
    TempDir user, builtin;
    PresetStore maker(builtin.path());
    Settings std_look;
    std_look.contrast = 30;
    std_look.shadows = 10;
    maker.save("Focal Default", std_look);
    PresetStore store(user.path(), builtin.path());
    Settings mine;
    mine.exposure = 0.5;
    const std::string mine_id = store.save("Mine", mine);

    // 切り取りなどがあっても、調整が同じなら一致する。同梱が先
    Settings s = std_look;
    s.geometry.rotate90 = 2;
    s.geometry.crop = {0, 0, 0.5, 0.5};
    REQUIRE(store.find_match(s));
    CHECK(*store.find_match(s) == "builtin:Focal Default");
    s.contrast = 31;
    CHECK_FALSE(store.find_match(s));
    CHECK(store.find_match(mine) == mine_id);

    // As Shot なら、色温度の古い値は見ない。カスタムなら見る
    Settings stale = std_look;
    stale.wb.temperature = 4000;
    CHECK(store.find_match(stale) == "builtin:Focal Default");
    Settings custom = std_look;
    custom.wb.mode = WhiteBalanceSettings::Mode::Custom;
    CHECK_FALSE(store.find_match(custom));

    // 保存すると、探す内容も新しくなる
    Settings fresh;
    fresh.clarity = 12;
    CHECK_FALSE(store.find_match(fresh));
    const std::string fresh_id = store.save("Fresh", fresh);
    CHECK(store.find_match(fresh) == fresh_id);
    store.remove(fresh_id);
    CHECK_FALSE(store.find_match(fresh));

    // 名前の変更: 中身は変わらない。同じ名前の別のプリセット・空の名前・同梱は不可
    store.rename(mine_id, "Mine 2");
    REQUIRE(store.list().size() == 2);
    CHECK(store.list()[1].name == "Mine 2");
    CHECK(store.find_match(mine) == mine_id);
    CHECK(store.load(mine_id).exposure == 0.5);
    store.save("Other", fresh);
    CHECK_THROWS_AS(store.rename(mine_id, "other"), Error);
    CHECK_THROWS_AS(store.rename(mine_id, "  "), Error);
    CHECK_THROWS_AS(store.rename("builtin:Focal Default", "X"), Error);
    CHECK_THROWS_AS(store.rename("user:none", "X"), Error);
    store.rename(mine_id, "MINE 2");  // 自分自身との大文字小文字だけの違いは許す
    CHECK(store.list()[1].name == "MINE 2");
}
