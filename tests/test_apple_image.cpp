// v3.22: macOS の ImageIO で HEIF を読む（macOS だけ）
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

#include "catalog/catalog.h"
#include "imaging/image_io.h"
#include "imaging/photo_file.h"
#include "imaging/raw_decoder.h"
#include "platform/apple_image_reader.h"
#include "test_util.h"
#include "thumbs/thumbnail.h"

using namespace focal;
namespace fs = std::filesystem;

TEST_CASE("macOS: HEIF を ImageIO で読む（撮影情報・大きさ・サムネイル）、カタログにも登録する", "[photo-file][apple]") {
    TempDir dir, db;
    ExifInfo e;
    e.capture_time = "2022-02-03T04:05:06";
    e.make = "Acme";
    e.model = "Phone 1";
    ImageU8 img(80, 40);
    for (size_t i = 0; i < img.data.size(); ++i) img.data[i] = static_cast<uint8_t>(i * 7);
    write_jpeg(dir / "src.jpg", img, 95, {}, &e);
    // sips（macOS 標準）で HEIC にする。変換できない環境では飛ばす
    const std::string cmd = "sips -s format heic '" + (dir / "src.jpg").string() + "' --out '" + (dir / "x.heic").string() +
                            "' >/dev/null 2>&1";
    if (std::system(cmd.c_str()) != 0 || !fs::exists(dir / "x.heic")) SKIP("sips で HEIC を作れない");

    set_platform_image_reader(nullptr);
    CHECK_FALSE(photo_kind_for_name("x.heic"));
    platform::register_apple_image_reader();
    REQUIRE(photo_kind_for_name("x.heic") == PhotoKind::Heif);

    const RawMetadata m = read_raw_metadata(dir / "x.heic");
    CHECK(m.width == 80);
    CHECK(m.height == 40);
    CHECK(m.make == "Acme");
    CHECK(capture_time_string(m.timestamp) == "2022-02-03T04:05:06");
    const Thumbnail t = make_thumbnail(dir / "x.heic");
    CHECK(t.image.width == 80);
    CHECK(t.image.height == 40);
    const DecodedRaw raw = decode_raw(dir / "x.heic");
    CHECK(raw.image.width == 80);

    auto c = Catalog::open(db / "c.sqlite");
    c->scan_root(c->add_root(dir.path()));
    // x.heic と src.jpg は別の名前なので、それぞれ別の写真
    CHECK(c->count(PhotoFilter{}) == 2);
    for (const auto& p : c->query(PhotoFilter{}))
        if (p.file_name == "x.heic") {
            CHECK(p.kind == PhotoKind::Heif);
            CHECK(p.status == PhotoStatus::Ok);
        }
}
