// Lensfun のレンズ DB の検索（v3.27）。DB は tools/fetch-lensfun-db.sh が build/lensfun/db に置く。なければ skip
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "imaging/lens_correction.h"
#include "imaging/lens_db.h"
#include "imaging/output_transform.h"
#include "imaging/renderer.h"

#include <filesystem>

using namespace focal;

#ifdef FOCAL_HAVE_LENSFUN
namespace {
const std::filesystem::path kDb = FOCAL_LENSFUN_DB_DIR;

RawMetadata canon_m50_wide() {
    RawMetadata m;
    m.make = "Canon";
    m.model = "Canon EOS M50";
    m.lens = "EF-M15-45mm f/3.5-6.3 IS STM";
    m.focal_length = 15;
    m.aperture = 3.5f;
    return m;
}
}

TEST_CASE("レンズ DB: 読み込みとカメラ・レンズの検索", "[lens]") {
    REQUIRE(LensDatabase::supported());
    if (!std::filesystem::is_directory(kDb)) SKIP("tools/fetch-lensfun-db.sh でレンズ DB を取得していない");

    LensDatabase db;
    REQUIRE(db.load_directory(kDb));
    CHECK(db.camera_count() > 500);
    CHECK(db.lens_count() > 1000);

    SECTION("存在しないディレクトリ") {
        LensDatabase empty;
        CHECK_FALSE(empty.load_directory(kDb / "no-such-dir"));
        CHECK(empty.lens_count() == 0);
    }
    SECTION("カメラの検索（クロップ係数・マウント）") {
        auto cam = db.find_camera("NIKON CORPORATION", "NIKON D850");
        REQUIRE(cam.has_value());
        CHECK(cam->mount == "Nikon F AF");
        CHECK(cam->crop_factor == 1.0f);
        CHECK_FALSE(db.find_camera("NIKON CORPORATION", "NO SUCH CAMERA 9999").has_value());
    }
    SECTION("EXIF のレンズ名から候補") {
        auto r = db.find_lenses("NIKON CORPORATION", "NIKON D850", "AF-S Nikkor 24-70mm f/2.8G ED");
        REQUIRE_FALSE(r.empty());
        CHECK(r[0].model.find("24-70") != std::string::npos);
        CHECK(r[0].has_distortion);
        CHECK(db.find_by_id(r[0].id).has_value());
    }
    SECTION("文字列検索") {
        auto r = db.search("sigma 35mm art");
        CHECK_FALSE(r.empty());
        for (const auto& c : r) CHECK(c.mounts.size() > 0);
        CHECK(db.search("zzzz-no-such-lens").empty());
        CHECK(db.search("nikkor", "Nikon Z", 5).size() <= 5);
    }
}
TEST_CASE("レンズ補正マップ: 歪曲・倍率色収差・周辺減光", "[lens]") {
    if (!std::filesystem::is_directory(kDb)) SKIP("tools/fetch-lensfun-db.sh でレンズ DB を取得していない");
    set_lens_database_dirs({kDb});
    const RawMetadata meta = canon_m50_wide();
    const int w = 6000, h = 4000;

    LensSettings ls;
    ls.enabled = true;

    SECTION("無効なら null") {
        LensSettings off;
        CHECK(build_lens_maps(off, meta, w, h) == nullptr);
        ls.distortion = ls.tca = ls.vignetting = 0;
        CHECK(build_lens_maps(ls, meta, w, h) == nullptr);
    }
    SECTION("レンズ名から自動で選ぶ") {
        std::optional<LensCandidate> resolved;
        auto maps = build_lens_maps(ls, meta, w, h, &resolved);
        REQUIRE(maps);
        REQUIRE(resolved.has_value());
        CHECK(resolved->model.find("15-45") != std::string::npos);
        CHECK(detect_lens(meta).has_value());
    }
    SECTION("レンズが見つからなければ null") {
        RawMetadata unknown = meta;
        unknown.lens = "no such lens 9999mm";
        CHECK(build_lens_maps(ls, unknown, w, h) == nullptr);
        ls.id = "No|Such";
        CHECK(build_lens_maps(ls, meta, w, h) == nullptr);
    }
    SECTION("中央はほぼ恒等、四隅は元の画像の内側に収まる（自動拡大）") {
        auto maps = build_lens_maps(ls, meta, w, h);
        REQUIRE(maps);
        CHECK(maps->has_geometry);
        float q[6], gain;
        maps->lookup(w / 2.0, h / 2.0, q, gain);
        for (int k = 0; k < 3; ++k) {
            CHECK(std::abs(q[k * 2] - w / 2.0) < 20.0);
            CHECK(std::abs(q[k * 2 + 1] - h / 2.0) < 20.0);
        }
        for (auto [px, py] : {std::pair{0.0, 0.0}, {double(w), 0.0}, {0.0, double(h)}, {double(w), double(h)},
                              {w / 2.0, 0.0}, {0.0, h / 2.0}}) {
            maps->lookup(px, py, q, gain);
            for (int k = 0; k < 3; ++k) {
                CHECK(q[k * 2] >= -2.0f);
                CHECK(q[k * 2] <= w + 2.0f);
                CHECK(q[k * 2 + 1] >= -2.0f);
                CHECK(q[k * 2 + 1] <= h + 2.0f);
            }
        }
    }
    SECTION("周辺減光: 四隅のゲインが 1 より大きく、量で変わる") {
        ls.distortion = 0;
        ls.tca = 0;
        auto full = build_lens_maps(ls, meta, w, h);
        REQUIRE(full);
        CHECK(full->has_gain);
        CHECK_FALSE(full->has_geometry);
        float q[6], g_center, g_corner;
        full->lookup(w / 2.0, h / 2.0, q, g_center);
        full->lookup(0, 0, q, g_corner);
        CHECK(std::abs(g_center - 1.0f) < 0.01f);
        CHECK(g_corner > 1.1f);
        ls.vignetting = 50;
        auto half = build_lens_maps(ls, meta, w, h);
        REQUIRE(half);
        float g_half;
        half->lookup(0, 0, q, g_half);
        CHECK(g_half == Catch::Approx(1.0f + (g_corner - 1.0f) * 0.5f).margin(0.01));
    }
    SECTION("倍率色収差: R・G・B の座標が四隅でずれる") {
        ls.distortion = 0;
        ls.vignetting = 0;
        auto maps = build_lens_maps(ls, meta, w, h);
        REQUIRE(maps);
        CHECK(maps->has_tca);
        float q[6], gain;
        maps->lookup(0, 0, q, gain);
        CHECK(std::abs(q[0] - q[4]) + std::abs(q[1] - q[5]) > 0.05f);
    }
    SECTION("歪曲収差の量 0 なら入力と同じ座標") {
        ls.distortion = 0;
        ls.tca = 0;
        ls.vignetting = 100;
        auto maps = build_lens_maps(ls, meta, w, h);
        REQUIRE(maps);
        float q[6], gain;
        maps->lookup(1234.5, 2345.5, q, gain);
        CHECK(q[2] == Catch::Approx(1234.5).margin(0.01));
        CHECK(q[3] == Catch::Approx(2345.5).margin(0.01));
    }
}

TEST_CASE("レンズ補正: 描画に反映される（補正なしは何も変わらない）", "[lens]") {
    if (!std::filesystem::is_directory(kDb)) SKIP("tools/fetch-lensfun-db.sh でレンズ DB を取得していない");
    set_lens_database_dirs({kDb});
    const RawMetadata meta = canon_m50_wide();
    const int w = 600, h = 400;
    // 全面が同じ明るさの画像: 周辺減光の補正で四隅だけ明るくなる
    ImageF proxy(w, h);
    for (auto& v : proxy.data) v = 0.2f;
    Settings st;
    st.lens.enabled = true;
    st.lens.distortion = 0;
    st.lens.tca = 0;
    const DecodedRaw raw = [&] {
        DecodedRaw r;
        r.image = ImageU16(w, h);
        r.meta = meta;
        return r;
    }();
    const OutputTransform xf(OutputSpace::Srgb, OutputDepth::U8);
    const ImageU8 on = render_preview(raw, proxy, st, xf);
    st.lens.enabled = false;
    const ImageU8 off = render_preview(raw, proxy, st, xf);
    REQUIRE(on.width == off.width);
    const int cx = w / 2, cy = h / 2;
    CHECK(on.row(cy)[cx * 3 + 1] == off.row(cy)[cx * 3 + 1]);
    CHECK(on.row(2)[2 * 3 + 1] > off.row(2)[2 * 3 + 1] + 3);
}
#else
TEST_CASE("レンズ DB: Lensfun なしのビルドでは空", "[lens]") {
    LensDatabase db;
    CHECK_FALSE(LensDatabase::supported());
    CHECK(db.lens_count() == 0);
    CHECK(db.search("nikkor").empty());
}
#endif
