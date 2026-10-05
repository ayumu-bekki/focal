// v3.22: RAW 以外の写真（JPEG・TIFF・PNG）の管理と、RAW と同じ名前のファイルを 1 枚として扱うこと
#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <set>

#include "catalog/catalog.h"
#include "catalog/photo_delete.h"
#include "edit/settings.h"
#include "imaging/image_io.h"
#include "imaging/photo_file.h"
#include "imaging/raw_decoder.h"
#include "import/card_import.h"
#include "test_util.h"
#include "thumbs/thumbnail.h"
#include "util/error.h"

using namespace focal;
namespace fs = std::filesystem;

namespace {

ImageU8 gradient(int w, int h) {
    ImageU8 img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = img.row(y) + x * 3;
            p[0] = static_cast<uint8_t>(x * 255 / std::max(1, w - 1));
            p[1] = static_cast<uint8_t>(y * 255 / std::max(1, h - 1));
            p[2] = 128;
        }
    return img;
}

ExifInfo sample_exif() {
    ExifInfo e;
    e.capture_time = "2021-03-04T05:06:07";
    e.make = "Acme";
    e.model = "Cam 1";
    e.lens = "50mm F1.8";
    e.iso = 400;
    e.exposure_time = 0.01;
    e.f_number = 2.8;
    e.focal_length = 50;
    return e;
}

void make_jpeg(const fs::path& p, int w = 64, int h = 48, bool with_exif = true) {
    fs::create_directories(p.parent_path());
    const ExifInfo e = sample_exif();
    write_jpeg(p, gradient(w, h), 90, {}, with_exif ? &e : nullptr);
}

void make_tiff(const fs::path& p, int w = 40, int h = 30) {
    fs::create_directories(p.parent_path());
    const ExifInfo e = sample_exif();
    write_tiff(p, gradient(w, h), {}, TiffCompression::None, &e);
}

// 6x4 の RGB PNG（EXIF なし）
void make_png(const fs::path& p) {
    static const unsigned char png[] = {
        0x89, 0x50, 0x4e, 0x47, 0xd,  0xa,  0x1a, 0xa,  0x0,  0x0,  0x0,  0xd,  0x49, 0x48, 0x44, 0x52, 0x0,  0x0,
        0x0,  0x6,  0x0,  0x0,  0x0,  0x4,  0x8,  0x2,  0x0,  0x0,  0x0,  0x22, 0x66, 0xd9, 0x14, 0x0,  0x0,  0x0,
        0x3c, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0xd,  0xc8, 0x41, 0x1,  0x0,  0x30, 0x8,  0x3,  0xb1, 0xca, 0x41,
        0x4,  0x22, 0x90, 0xd3, 0xe7, 0x49, 0x41, 0x44, 0x45, 0x20, 0x6b, 0xcb, 0x33, 0x92, 0x28, 0x31, 0xc2, 0x62,
        0xc5, 0x9,  0xa9, 0xa9, 0x66, 0x1a, 0x37, 0xdb, 0x5c, 0xff, 0x32, 0x65, 0xc6, 0xd8, 0xac, 0x39, 0xff, 0xa,
        0x15, 0x26, 0x38, 0x6c, 0xb8, 0xf0, 0x0,  0xc4, 0x51, 0x1d, 0xd1, 0xbf, 0xa4, 0x58, 0x84, 0x0,  0x0,  0x0,
        0x0,  0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(png), sizeof png);
}

// 中身は RAW として読めない（「非対応」の行になる）が、RAW の拡張子なので RAW の写真として登録される
void make_fake_raw(const fs::path& p, const std::string& content = "not a real raw") {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}

std::optional<PhotoRecord> photo_named(Catalog& c, const std::string& name) {
    for (const auto& p : c.query(PhotoFilter{}))
        if (p.file_name == name) return p;
    return std::nullopt;
}

std::set<std::string> names(Catalog& c) {
    std::set<std::string> out;
    for (const auto& p : c.query(PhotoFilter{})) out.insert(p.file_name);
    return out;
}

} // namespace

TEST_CASE("拡張子から写真の種類を決める（大文字小文字を区別しない）", "[photo-file]") {
    CHECK(photo_kind_for_name("A.CR3") == PhotoKind::Raw);
    CHECK(photo_kind_for_name("a.NEF") == PhotoKind::Raw);
    CHECK(photo_kind_for_name("a.jpg") == PhotoKind::Jpeg);
    CHECK(photo_kind_for_name("a.JPEG") == PhotoKind::Jpeg);
    CHECK(photo_kind_for_name("a.Tif") == PhotoKind::Tiff);
    CHECK(photo_kind_for_name("a.tiff") == PhotoKind::Tiff);
    CHECK(photo_kind_for_name("a.PNG") == PhotoKind::Png);
    CHECK_FALSE(photo_kind_for_name("a.xmp"));
    CHECK_FALSE(photo_kind_for_name("a.mov"));
    CHECK_FALSE(photo_kind_for_name("noext"));
    // HEIF は、OS の読み取り部品が登録されているときだけ
    set_platform_image_reader(nullptr);
    CHECK_FALSE(photo_kind_for_name("a.heic"));
    set_platform_image_reader([](const fs::path&, int) { return PlatformImage{}; });
    CHECK(photo_kind_for_name("a.HEIC") == PhotoKind::Heif);
    CHECK(photo_kind_for_name("a.heif") == PhotoKind::Heif);
    set_platform_image_reader(nullptr);

    // EXIF の Orientation → LibRaw の flip
    const int expect[] = {0, 0, 1, 3, 2, 4, 6, 7, 5};
    for (int o = 1; o <= 8; ++o) CHECK(flip_from_exif_orientation(o) == expect[o]);
    CHECK(flip_from_exif_orientation(0) == 0);
    CHECK(flip_from_exif_orientation(9) == 0);
}

TEST_CASE("JPEG・TIFF・PNG: 撮影情報・大きさ・サムネイル・現像の土台", "[photo-file]") {
    TempDir dir;
    make_jpeg(dir / "a.jpg", 1000, 500);
    make_tiff(dir / "b.tif", 40, 30);
    make_png(dir / "c.png");

    for (const char* name : {"a.jpg", "b.tif"}) {
        const RawMetadata m = read_raw_metadata(dir / name);  // RAW 以外は拡張子で別の経路
        CHECK(m.make == "Acme");
        CHECK(m.model == "Cam 1");
        CHECK(capture_time_string(m.timestamp) == "2021-03-04T05:06:07");
    }
    const RawMetadata j = read_raw_metadata(dir / "a.jpg");
    CHECK(j.iso == 400);  // 書き出しの TIFF は EXIF の IFD（露出など）までは持たない。JPEG は持つ
    CHECK(j.aperture == 2.8f);
    CHECK(j.focal_length == 50.0f);
    CHECK(j.width == 1000);
    CHECK(j.height == 500);
    CHECK(j.lens == "50mm F1.8");
    const RawMetadata t = read_raw_metadata(dir / "b.tif");
    CHECK(t.width == 40);
    CHECK(t.height == 30);
    const RawMetadata p = read_raw_metadata(dir / "c.png");
    CHECK(p.width == 6);
    CHECK(p.height == 4);
    CHECK(p.timestamp == 0);  // EXIF がなければ撮影日時は不明

    // サムネイル: 長辺 512、sRGB
    const Thumbnail tj = make_thumbnail(dir / "a.jpg");
    CHECK(std::max(tj.image.width, tj.image.height) == 512);
    CHECK(tj.image.width == 512);
    CHECK(tj.image.height == 256);
    const Thumbnail tp = make_thumbnail(dir / "c.png");
    CHECK(tp.image.width == 6);
    CHECK(tp.image.height == 4);
    // PNG の画素: 1 行目の x = 1 は (40, 0, 128)
    CHECK(tp.image.row(0)[1 * 3 + 0] == 40);
    CHECK(tp.image.row(0)[1 * 3 + 2] == 128);

    // 現像の土台: 色の行列は恒等。既定の設定でレンダリングしても、元の画像とほぼ同じ
    const DecodedRaw raw = decode_raw(dir / "b.tif");
    CHECK(raw.image.width == 40);
    CHECK(raw.image.height == 30);
    CHECK(raw.color.rgb_cam.m[0][0] == 1.0);
    CHECK(raw.color.as_shot_wb[0] == 1.0);
    ThumbnailOptions rendered;
    rendered.settings = Settings{};
    const Thumbnail via_pipeline = make_thumbnail(dir / "b.tif", rendered);
    const Thumbnail direct = make_thumbnail(dir / "b.tif");
    REQUIRE(via_pipeline.image.width == direct.image.width);
    REQUIRE(via_pipeline.image.height == direct.image.height);
    int worst = 0;
    for (size_t i = 0; i < direct.image.data.size(); ++i)
        worst = std::max(worst, std::abs(int(direct.image.data[i]) - int(via_pipeline.image.data[i])));
    CHECK(worst <= 3);

    // 壊れたファイル
    std::ofstream(dir / "bad.jpg") << "not a jpeg";
    CHECK_THROWS_AS(read_raw_metadata(dir / "bad.jpg"), Error);
}

TEST_CASE("カタログ: RAW と同じ名前の JPEG は 1 枚（付属ファイル）、RAW がなければそれぞれ別の写真", "[photo-file][catalog]") {
    TempDir lib, db;
    make_fake_raw(lib / "d" / "A.CR3");
    make_jpeg(lib / "d" / "a.jpg");      // 大文字小文字が違っても同じ名前
    make_tiff(lib / "d" / "A.tif");      // RAW があれば TIFF も付属ファイル
    make_jpeg(lib / "d" / "B.JPG");      // JPEG だけの 1 枚
    make_png(lib / "d" / "C.png");
    make_jpeg(lib / "d" / "C.jpg");      // RAW がない同名（PNG と JPEG）は別の写真
    make_fake_raw(lib / "d" / "E.CR3");  // RAW 同士は別の写真（同じ名前の DNG でも）
    make_fake_raw(lib / "d" / "E.DNG");
    std::ofstream(lib / "d" / "a.xmp") << "sidecar";  // 写真ではない

    auto c = Catalog::open(db / "c.sqlite");
    const int64_t root = c->add_root(lib.path());
    const ScanStats st = c->scan_root(root);
    CHECK(st.added == 6);
    CHECK(names(*c) == std::set<std::string>{"A.CR3", "B.JPG", "C.png", "C.jpg", "E.CR3", "E.DNG"});

    const auto a = photo_named(*c, "A.CR3");
    REQUIRE(a);
    CHECK(a->kind == PhotoKind::Raw);
    CHECK(a->companions == std::vector<std::string>{"A.tif", "a.jpg"});
    CHECK(photo_named(*c, "B.JPG")->kind == PhotoKind::Jpeg);
    CHECK(photo_named(*c, "B.JPG")->companions.empty());
    CHECK(photo_named(*c, "B.JPG")->status == PhotoStatus::Ok);
    CHECK(photo_named(*c, "B.JPG")->width == 64);
    CHECK(photo_named(*c, "B.JPG")->capture_time == "2021-03-04T05:06:07");
    CHECK(photo_named(*c, "C.png")->kind == PhotoKind::Png);
    CHECK(photo_named(*c, "E.CR3")->companions.empty());

    // もう一度スキャンしても同じ（変更なし）
    const ScanStats again = c->scan_root(root);
    CHECK(again.added == 0);
    CHECK(again.missing == 0);
    CHECK(again.unchanged == 6);

    // 付属ファイルを増やす・減らす: 行は増えず、付属ファイルの記録だけ変わる
    make_jpeg(lib / "d" / "E.jpg");
    fs::remove(lib / "d" / "A.tif");
    c->scan_root(root);
    CHECK(names(*c).size() == 6);
    CHECK(photo_named(*c, "A.CR3")->companions == std::vector<std::string>{"a.jpg"});
    CHECK(photo_named(*c, "E.CR3")->companions == std::vector<std::string>{"E.jpg"});
}

TEST_CASE("カタログ: RAW がなくなって JPEG だけが残ると、同じ写真（★・タグ）のまま JPEG の写真になり、RAW が戻ると RAW の写真に戻る",
          "[photo-file][catalog]") {
    TempDir lib, db;
    make_fake_raw(lib / "A.CR3");
    make_jpeg(lib / "A.JPG");
    auto c = Catalog::open(db / "c.sqlite");
    const int64_t root = c->add_root(lib.path());
    c->scan_root(root);
    REQUIRE(c->count(PhotoFilter{}) == 1);
    const int64_t id = photo_named(*c, "A.CR3")->id;
    c->set_rating(std::vector<int64_t>{id}, 4);
    c->add_tag(std::vector<int64_t>{id}, c->ensure_tag("trip"));
    c->save_edit(id, 1, std::string(R"({"schema":1,"processVersion":1,"exposure":0.5})"));
    c->flush();

    // RAW を消す
    fs::remove(lib / "A.CR3");
    c->scan_root(root);
    REQUIRE(c->count(PhotoFilter{}) == 1);
    auto p = c->photo(id);
    REQUIRE(p);
    CHECK(p->file_name == "A.JPG");
    CHECK(p->kind == PhotoKind::Jpeg);
    CHECK(p->status == PhotoStatus::Ok);
    CHECK(p->rating == 4);
    CHECK(p->companions.empty());

    // RAW が戻る
    make_fake_raw(lib / "A.CR3");
    c->scan_root(root);
    REQUIRE(c->count(PhotoFilter{}) == 1);
    p = c->photo(id);
    REQUIRE(p);
    CHECK(p->file_name == "A.CR3");
    CHECK(p->kind == PhotoKind::Raw);
    CHECK(p->rating == 4);
    CHECK(p->companions == std::vector<std::string>{"A.JPG"});
}

TEST_CASE("カタログ: JPEG を先に登録してから RAW が来ても、1 枚にまとまる。RAW の行が先にあれば JPEG の行を吸収する", "[photo-file][catalog]") {
    TempDir lib, db;
    make_jpeg(lib / "A.JPG");
    make_jpeg(lib / "B.JPG");
    auto c = Catalog::open(db / "c.sqlite");
    const int64_t root = c->add_root(lib.path());
    c->scan_root(root);
    REQUIRE(c->count(PhotoFilter{}) == 2);
    const int64_t a = photo_named(*c, "A.JPG")->id;
    c->set_rating(std::vector<int64_t>{a}, 3);
    c->flush();

    make_fake_raw(lib / "A.CR3");
    c->scan_root(root);
    CHECK(c->count(PhotoFilter{}) == 2);
    const auto p = c->photo(a);
    REQUIRE(p);
    CHECK(p->file_name == "A.CR3");
    CHECK(p->rating == 3);
    CHECK(p->companions == std::vector<std::string>{"A.JPG"});

    // RAW の行（ファイルなし）と JPEG の行が別々にあって、RAW が戻ってきた: JPEG の行は RAW の行に吸収される
    fs::remove(lib / "A.CR3");
    make_fake_raw(lib / "B.CR3");  // B にも RAW を足す
    c->scan_root(root);
    c->set_rating(std::vector<int64_t>{photo_named(*c, "B.CR3")->id}, 2);
    c->flush();
    CHECK(c->count(PhotoFilter{}) == 2);
}

TEST_CASE("削除: 主役が RAW なら同じ名前の JPEG も一緒に消し、主役が JPEG なら同じ名前の別の写真は消さない", "[photo-file][delete]") {
    TempDir lib, db;
    make_fake_raw(lib / "A.CR3");
    make_jpeg(lib / "A.JPG");
    std::ofstream(lib / "A.xmp") << "x";
    make_jpeg(lib / "B.jpg");
    make_png(lib / "B.png");
    auto c = Catalog::open(db / "c.sqlite");
    c->scan_root(c->add_root(lib.path()));
    REQUIRE(c->count(PhotoFilter{}) == 3);

    const int64_t a = photo_named(*c, "A.CR3")->id, bj = photo_named(*c, "B.jpg")->id;
    const DeletePlan plan_a = plan_delete(*c, std::vector<int64_t>{a});
    REQUIRE(plan_a.items.size() == 1);
    CHECK(plan_a.items[0].companions.size() == 2);  // A.JPG と A.xmp
    const DeletePlan plan_b = plan_delete(*c, std::vector<int64_t>{bj});
    REQUIRE(plan_b.items.size() == 1);
    CHECK(plan_b.items[0].companions.empty());  // B.png は別の写真
}

TEST_CASE("現像のプリセットは、RAW 以外の写真には重ねない", "[photo-file][preset]") {
    TempDir lib, db;
    make_jpeg(lib / "A.jpg");
    make_fake_raw(lib / "B.CR3");
    auto c = Catalog::open(db / "c.sqlite");
    c->scan_root(c->add_root(lib.path()));
    Settings preset;
    preset.contrast = 20;
    const std::vector<int64_t> ids = {photo_named(*c, "A.jpg")->id, photo_named(*c, "B.CR3")->id};
    c->apply_preset(ids, preset);
    c->flush();
    CHECK_FALSE(c->edit_json(ids[0]));
    CHECK(c->edit_json(ids[1]));
}

TEST_CASE("カード取り込み: JPEG だけの 1 枚も登録し、RAW と同じ名前の JPEG は付属ファイルとして一緒に記録する", "[photo-file][import]") {
    TempDir card, lib, db;
    make_jpeg(card / "DCIM/100X/IMG_1.JPG");   // JPEG だけ（撮影日時は EXIF から）
    make_fake_raw(card / "DCIM/100X/IMG_2.CR3");
    make_jpeg(card / "DCIM/100X/IMG_2.JPG");   // RAW + JPEG のペア
    auto c = Catalog::open(db / "c.sqlite");
    CardImportOptions opt;
    opt.source = card.path();
    opt.dest_root = lib / "Photos";
    Settings preset;
    preset.contrast = 10;
    opt.preset = preset;
    const CardImportResult r = import_from_card(*c, opt);
    CHECK(r.shots == 2);
    CHECK(r.imported == 2);
    CHECK(r.estimated_dates == 1);  // RAW（中身が読めない）は推定。JPEG は EXIF の日時
    CHECK(fs::exists(lib / "Photos" / "2021" / "2021-03-04" / "IMG_1.JPG"));
    CHECK(r.photo_ids.size() == 2);
    CHECK(c->count(PhotoFilter{}) == 2);
    const auto one = photo_named(*c, "IMG_1.JPG");
    REQUIRE(one);
    CHECK(one->kind == PhotoKind::Jpeg);
    CHECK(one->capture_time == "2021-03-04T05:06:07");
    const auto two = photo_named(*c, "IMG_2.CR3");
    REQUIRE(two);
    CHECK(two->companions == std::vector<std::string>{"IMG_2.JPG"});
    c->flush();
    CHECK_FALSE(c->edit_json(one->id));  // プリセットは RAW 以外には重ねない
    CHECK(c->edit_json(two->id));

    // もう一度取り込むと、取り込み済み
    CHECK(import_from_card(*c, opt).skipped_duplicates == 2);
}
