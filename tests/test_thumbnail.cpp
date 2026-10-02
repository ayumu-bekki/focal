#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>

#include "imaging/geometry.h"
#include "imaging/raw_decoder.h"
#include "test_util.h"
#include "thumbs/thumbnail.h"

using namespace focal;
namespace fs = std::filesystem;

TEST_CASE("サムネイル: 向き補正はジオメトリの flip と同じ対応", "[thumbs]") {
    const int flip = GENERATE(0, 3, 5, 6, 1, 2, 4, 7);
    ImageU8 src(7, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 7; ++x) {
            src.row(y)[x * 3] = static_cast<uint8_t>(x);
            src.row(y)[x * 3 + 1] = static_cast<uint8_t>(y);
        }
    const ImageU8 out = apply_orientation(src, flip);
    const GeometryPlan plan(7, 4, flip, GeometrySettings{});
    REQUIRE(out.width == plan.output_width());
    REQUIRE(out.height == plan.output_height());
    const Affine m = plan.output_to_sensor(1.0);
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            const PointD s = m.apply({x + 0.5, y + 0.5});
            REQUIRE(out.row(y)[x * 3] == static_cast<int>(s.x));
            REQUIRE(out.row(y)[x * 3 + 1] == static_cast<int>(s.y));
        }
}

TEST_CASE("サムネイル: 面積平均の縮小", "[thumbs]") {
    ImageU8 src(1000, 500);
    for (size_t i = 0; i < src.data.size(); ++i) src.data[i] = (i / 3) % 2 ? 200 : 100;
    const ImageU8 out = downscale_u8(src, 512);
    CHECK(out.width == 512);
    CHECK(out.height == 256);
    CHECK(std::abs(int(out.row(100)[300]) - 150) <= 2);
    CHECK(downscale_u8(out, 1024).width == 512);  // 拡大はしない
}

TEST_CASE("サムネイル: キャッシュのキーと保存", "[thumbs]") {
    const std::string k = thumbnail_key("/a/b.CR3", 100, 200);
    CHECK(k.size() == 64);
    CHECK(thumbnail_key("/a/b.CR3", 100, 200) == k);
    CHECK(thumbnail_key("/a/b.CR3", 100, 201) != k);
    CHECK(thumbnail_key("/a/b.CR3", 101, 200) != k);
    CHECK(thumbnail_key("/a/c.CR3", 100, 200) != k);
    CHECK(thumbnail_key("/a/b.CR3", 100, 200, "settings") != k);

    TempDir dir;
    const ThumbnailCache cache(dir / "thumbs");
    CHECK_FALSE(cache.contains(k));
    cache.store(k, ImageU8(16, 8));
    CHECK(cache.contains(k));
    CHECK(cache.path_for(k).parent_path().filename() == k.substr(0, 2));
}

TEST_CASE("サムネイル: 5 機種で埋め込みとレンダリングの両方が作れ、向きと縦横比が合う", "[thumbs][data]") {
    const char* name = GENERATE("canon_eos_m50.CR3", "nikon_z7.NEF", "sony_ilce7m3.ARW", "fujifilm_xt3.RAF",
                                "ricoh_gr3.DNG");
    const fs::path path = fs::path(FOCAL_TEST_DATA_DIR) / name;
    if (!fs::exists(path)) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");
    INFO(name);
    const RawMetadata meta = read_raw_metadata(path);
    const double aspect = static_cast<double>(meta.width) / meta.height;

    const Thumbnail embedded = make_thumbnail(path);
    CHECK(embedded.source == ThumbnailSource::Embedded);
    CHECK(std::max(embedded.image.width, embedded.image.height) == kThumbnailLongEdge);
    CHECK(std::abs(static_cast<double>(embedded.image.width) / embedded.image.height - aspect) < 0.02);

    ThumbnailOptions opt;
    opt.allow_embedded = false;
    const Thumbnail rendered = make_thumbnail(path, opt);
    CHECK(rendered.source == ThumbnailSource::Rendered);
    CHECK(std::max(rendered.image.width, rendered.image.height) == kThumbnailLongEdge);
    CHECK(std::abs(static_cast<double>(rendered.image.width) / rendered.image.height - aspect) < 0.02);
}

TEST_CASE("サムネイル: 大きいプレビューのキャッシュは上限を超えると古く使ったものから消す", "[thumbs]") {
    TempDir dir;
    ImageU8 img(64, 48);
    for (size_t i = 0; i < img.data.size(); ++i) img.data[i] = static_cast<uint8_t>(i * 37);
    PreviewCache cache(dir.path() / "previews", 1ull << 30);
    cache.store("aa01", img);
    const uint64_t one = cache.usage();
    REQUIRE(one > 0);
    cache.store("bb02", img);
    cache.store("cc03", img);
    // aa01 を一番古く、bb02 を使ったばかりにする
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(dir.path() / "previews" / "aa" / "aa01.jpg", now - std::chrono::hours(3));
    std::filesystem::last_write_time(dir.path() / "previews" / "cc" / "cc03.jpg", now - std::chrono::hours(2));
    std::filesystem::last_write_time(dir.path() / "previews" / "bb" / "bb02.jpg", now - std::chrono::hours(1));
    ImageU8 out;
    REQUIRE(cache.load("aa01", out));  // 読むと使った日時が新しくなる
    CHECK(out.width == 64);
    CHECK(out.height == 48);

    cache.set_limit(one * 2);
    CHECK(cache.contains("aa01"));
    CHECK(cache.contains("bb02"));
    CHECK_FALSE(cache.contains("cc03"));
    CHECK(cache.usage() <= one * 2);

    cache.clear();
    CHECK(cache.usage() == 0);
    CHECK_FALSE(cache.load("aa01", out));
}
