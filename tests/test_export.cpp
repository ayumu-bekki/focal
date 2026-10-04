#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>

#include "catalog/catalog.h"
#include "edit/settings.h"
#include "export/exporter.h"
#include "imaging/image_io.h"
#include "imaging/output_transform.h"
#include "test_util.h"
#include "util/error.h"
#include "util/file.h"

using namespace focal;
namespace fs = std::filesystem;

namespace {

const std::string kNfc = "\xe3\x81\x8c\xe3\x81\xa3\xe3\x81\x93\xe3\x81\x86";  // がっこう
const std::string kNfd = "\xe3\x81\x8b\xe3\x82\x99\xe3\x81\xa3\xe3\x81\x93\xe3\x81\x86";

// ICC のヘッダ（先頭 128 バイト）には作成日時とそれから計算する ID が入るので、タグの中身で比べる
bool same_profile(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    return a.size() == b.size() && a.size() > 128 && std::equal(a.begin() + 128, a.end(), b.begin() + 128) &&
           std::equal(a.begin() + 16, a.begin() + 24, b.begin() + 16);  // 色空間と PCS
}

bool have_data() { return fs::exists(fs::path(FOCAL_TEST_DATA_DIR) / "sony_ilce7m3.ARW"); }

struct Fixture {
    TempDir dir{"focal-export"};
    std::unique_ptr<Catalog> catalog;
    int64_t sony = 0;
    fs::path out = dir / "out";

    Fixture() {
        fs::create_directories(dir / "lib");
        fs::create_directories(out);
        // 日本語（NFD）の名前
        fs::copy_file(fs::path(FOCAL_TEST_DATA_DIR) / "sony_ilce7m3.ARW", dir / "lib" / utf8_to_path(kNfd + ".ARW"));
        catalog = Catalog::open(dir / "c.sqlite");
        catalog->scan_root(catalog->add_root(dir / "lib"));
        sony = catalog->query({}).at(0).id;
    }

    std::vector<std::string> outputs() const {
        std::vector<std::string> v;
        for (auto& e : fs::directory_iterator(out)) v.push_back(path_to_utf8(e.path().filename()));
        std::sort(v.begin(), v.end());
        return v;
    }
};

} // namespace

TEST_CASE("書き出し先の名前: 拡張子を付け替え、あれば _1, _2 を付ける", "[export]") {
    TempDir dir;
    CHECK(reserve_output_path(dir.path(), "IMG_0001", "jpg").filename() == "IMG_0001.jpg");
    CHECK(reserve_output_path(dir.path(), "IMG_0001", "jpg").filename() == "IMG_0001_1.jpg");
    CHECK(reserve_output_path(dir.path(), "IMG_0001", "jpg").filename() == "IMG_0001_2.jpg");
    CHECK(reserve_output_path(dir.path(), "IMG_0001", "tif").filename() == "IMG_0001.tif");
    CHECK_THROWS_AS(reserve_output_path(dir / "missing", "a", "jpg"), Error);
}

TEST_CASE("書き出し: JPEG を読み戻して ICC・ビット深度・サイズを確認する", "[export][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Fixture f;
    ExportOptions opt;
    opt.dest_dir = f.out;
    opt.long_edge = 1024;
    opt.quality = 90;
    const ExportItemResult r = export_photo(*f.catalog, f.sony, opt);
    REQUIRE(r.ok);
    // 名前は元のファイル名（NFC）の拡張子を付け替えたもの
    CHECK(path_to_utf8(r.output.filename()) == kNfc + ".jpg");

    const LoadedImage img = read_jpeg(r.output);
    CHECK(img.width == 1024);
    CHECK(img.height == 684);
    CHECK(img.bits == 8);
    CHECK(same_profile(img.icc, OutputTransform(OutputSpace::Srgb, OutputDepth::U8).icc_profile()));

    // EXIF: 撮影日時・カメラ・露出がカタログの値と同じ（外部のソフトが日時で並べられる）
    const auto photo = f.catalog->photo(f.sony);
    REQUIRE(photo.has_value());
    const auto exif = read_jpeg_exif(r.output);
    REQUIRE(exif.has_value());
    CHECK(exif->capture_time == *photo->capture_time);
    CHECK(exif->capture_time == "2018-03-13T16:38:13");
    CHECK(exif->make == photo->camera_make);
    CHECK(exif->model == photo->camera_model);
    CHECK(exif->lens == photo->lens_model);
    CHECK(exif->iso == photo->iso);
    REQUIRE(exif->exposure_time.has_value());
    CHECK(std::abs(*exif->exposure_time - *photo->exposure_time) < 0.0005);  // 1/50
    REQUIRE(exif->f_number.has_value());
    CHECK(std::abs(*exif->f_number - *photo->f_number) < 0.05);
    REQUIRE(exif->focal_length.has_value());
    CHECK(std::abs(*exif->focal_length - *photo->focal_length) < 0.05);

    // もう一度書き出すと上書きせず _1 が付く
    const ExportItemResult r2 = export_photo(*f.catalog, f.sony, opt);
    REQUIRE(r2.ok);
    CHECK(path_to_utf8(r2.output.filename()) == kNfc + "_1.jpg");
}

TEST_CASE("書き出し: TIFF にも撮影日時・メーカー・機種を書く", "[export][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Fixture f;
    ExportOptions opt;
    opt.dest_dir = f.out;
    opt.format = ExportOptions::Format::Tiff16;
    opt.long_edge = 512;
    const ExportItemResult r = export_photo(*f.catalog, f.sony, opt);
    REQUIRE(r.ok);
    const auto exif = read_tiff_exif(r.output);
    REQUIRE(exif.has_value());
    CHECK(exif->capture_time == "2018-03-13T16:38:13");
    CHECK(exif->make == f.catalog->photo(f.sony)->camera_make);
    CHECK(exif->model == f.catalog->photo(f.sony)->camera_model);
}

TEST_CASE("書き出し: 16-bit TIFF（原寸）と編集の反映", "[export][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Fixture f;
    ExportOptions opt;
    opt.dest_dir = f.out;
    opt.format = ExportOptions::Format::Tiff16;
    const ExportItemResult r = export_photo(*f.catalog, f.sony, opt);
    REQUIRE(r.ok);
    const LoadedImage img = read_tiff(r.output);
    CHECK(img.width == 6024);
    CHECK(img.height == 4024);
    CHECK(img.bits == 16);
    CHECK_FALSE(img.icc.empty());
    CHECK(same_profile(img.icc, OutputTransform(OutputSpace::Srgb, OutputDepth::U16).icc_profile()));

    // 回転とクロップを保存してから書き出すと、縦長でクロップした大きさになる
    Settings s;
    s.geometry.rotate90 = 1;
    s.geometry.crop = {0.1, 0.1, 0.5, 0.5};
    f.catalog->save_edit(f.sony, 1, settings_to_json(s));
    f.catalog->flush();
    opt.format = ExportOptions::Format::Jpeg;
    opt.long_edge = 0;
    const ExportItemResult e = export_photo(*f.catalog, f.sony, opt);
    REQUIRE(e.ok);
    const LoadedImage ei = read_jpeg(e.output);
    CHECK(ei.width == 2012);   // 4024 × 0.5
    CHECK(ei.height == 3012);  // 6024 × 0.5
}

TEST_CASE("書き出し: 失敗は 1 枚ごと、キャンセルすると残りは書き出さず書きかけも残らない", "[export][data]") {
    if (!have_data()) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    Fixture f;
    ExportOptions opt;
    opt.dest_dir = f.out;
    opt.long_edge = 512;

    // 存在しない写真と、書き出せる写真
    std::vector<ExportItemResult> results;
    CHECK(export_photos(*f.catalog, {99999, f.sony}, opt,
                        [&](int, int, const ExportItemResult& r) { results.push_back(r); }));
    REQUIRE(results.size() == 2);
    CHECK_FALSE(results[0].ok);
    CHECK_FALSE(results[0].error.empty());
    CHECK(results[1].ok);

    // 1 枚目が終わったらキャンセル
    std::atomic<bool> cancel{false};
    int calls = 0;
    const bool completed = export_photos(*f.catalog, {f.sony, f.sony, f.sony}, opt,
                                         [&](int, int, const ExportItemResult&) {
                                             ++calls;
                                             cancel = true;
                                         },
                                         &cancel);
    CHECK_FALSE(completed);
    CHECK(calls == 1);
    // 書き出したのは最初の 1 回目と今回の 1 枚だけ。空のファイルは残っていない
    const auto files = f.outputs();
    CHECK(files == std::vector<std::string>{kNfc + ".jpg", kNfc + "_1.jpg"});
    for (const auto& name : files) CHECK(fs::file_size(f.out / utf8_to_path(name)) > 1000);

    // 書き出し先がなければ 1 枚ごとの失敗になり、何も作らない
    opt.dest_dir = f.dir / "missing";
    const ExportItemResult m = export_photo(*f.catalog, f.sony, opt);
    CHECK_FALSE(m.ok);
    CHECK_FALSE(fs::exists(f.dir / "missing"));
}
