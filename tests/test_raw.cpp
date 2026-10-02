#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <libraw/libraw.h>

#include <filesystem>
#include <memory>

#include <cmath>

#include "imaging/geometry.h"
#include "imaging/output_transform.h"
#include "imaging/raw_decoder.h"
#include "imaging/renderer.h"
#include "imaging/resample.h"

using namespace focal;
namespace fs = std::filesystem;

namespace {

fs::path data_file(const char* name) { return fs::path(FOCAL_TEST_DATA_DIR) / name; }

} // namespace

TEST_CASE("RAW: デコード結果の基本的な性質", "[raw][data]") {
    const char* name = GENERATE("canon_eos_m50.CR3", "nikon_z7.NEF", "sony_ilce7m3.ARW", "fujifilm_xt3.RAF",
                                "ricoh_gr3.DNG");
    const fs::path path = data_file(name);
    if (!fs::exists(path)) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する: " << name);

    const DecodedRaw raw = decode_raw(path, {.half_size = true});
    INFO(name);
    CHECK(raw.image.width > 1000);
    CHECK(raw.image.height > 600);
    CHECK(raw.image.data.size() == raw.image.pixel_count() * 3);
    // バッファは向き未適用: メタデータ（向き補正後）のサイズとは flip に応じて対応する
    const int full_w = (raw.flip & 4) ? raw.meta.height : raw.meta.width;
    CHECK(std::abs(raw.image.width * 2 - full_w) <= 2);
    CHECK(raw.color.as_shot_wb[1] == 1.0);
    CHECK(raw.color.as_shot_wb[0] > 0.3);
    CHECK(raw.color.as_shot_wb[0] < 5.0);
    CHECK(raw.color.as_shot_wb[2] > 0.3);
    CHECK(raw.color.as_shot_wb[2] < 5.0);
    CHECK(!raw.meta.make.empty());
}

TEST_CASE("RAW: imgdata.image は向き未適用で、自前の flip 適用が LibRaw の出力と一致する", "[raw][data]") {
    const fs::path path = data_file("sony_ilce7m3.ARW");
    if (!fs::exists(path)) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");

    // flip を強制的に 6（時計回り 90°）にして LibRaw に回転済み画像を作らせ、自前の対応と比べる
    auto raw = std::make_unique<LibRaw>();
    auto& p = raw->imgdata.params;
    p.output_color = 0;
    p.output_bps = 16;
    p.gamm[0] = p.gamm[1] = 1.0;
    p.no_auto_bright = 1;
    p.use_camera_wb = 1;
    p.half_size = 1;
    p.user_flip = 6;
    REQUIRE(raw->open_file(path.string().c_str()) == LIBRAW_SUCCESS);  // テストのパスは ASCII のみ
    REQUIRE(raw->unpack() == LIBRAW_SUCCESS);
    REQUIRE(raw->dcraw_process() == LIBRAW_SUCCESS);

    const int iw = raw->imgdata.sizes.iwidth, ih = raw->imgdata.sizes.iheight;
    // flip 6 でも imgdata.image は横長（センサーの向き）のまま
    CHECK(iw > ih);
    CHECK(raw->imgdata.sizes.flip == 6);

    // 比較用に imgdata.image を取っておく（dcraw_make_mem_image がカーブを作る）
    std::vector<uint16_t> image(static_cast<size_t>(iw) * ih * 3);
    for (size_t i = 0; i < static_cast<size_t>(iw) * ih; ++i)
        for (int c = 0; c < 3; ++c) image[i * 3 + c] = raw->imgdata.image[i][c];

    int err = 0;
    libraw_processed_image_t* mem = raw->dcraw_make_mem_image(&err);
    REQUIRE(mem != nullptr);
    REQUIRE(mem->bits == 16);
    CHECK(mem->width == ih);  // 回転済みなので縦横が入れ替わる
    CHECK(mem->height == iw);
    const auto* curve = raw->imgdata.color.curve;
    const auto* out = reinterpret_cast<const uint16_t*>(mem->data);

    const GeometryPlan plan(iw, ih, 6, GeometrySettings{});
    const Affine m = plan.output_to_sensor(1.0);
    size_t mismatches = 0;
    for (int y = 0; y < mem->height; y += 7)
        for (int x = 0; x < mem->width; x += 7) {
            const PointD s = m.apply({x + 0.5, y + 0.5});
            const size_t si = static_cast<size_t>(s.y) * iw + static_cast<size_t>(s.x);
            for (int c = 0; c < 3; ++c)
                if (out[(static_cast<size_t>(y) * mem->width + x) * 3 + c] != curve[image[si * 3 + c]]) ++mismatches;
        }
    LibRaw::dcraw_clear_mem(mem);
    CHECK(mismatches == 0);
}

TEST_CASE("一致性: プロキシのレンダリングと、書き出しを同サイズに縮小したものがほぼ一致する", "[raw][data]") {
    const fs::path path = data_file("nikon_z7.NEF");
    if (!fs::exists(path)) SKIP("tests/data/fetch.sh でテスト用 RAW を取得する");

    const DecodedRaw raw = decode_raw(path, {.half_size = true});
    Settings s;
    s.exposure = 0.3;
    s.contrast = 20;
    s.highlights = -40;
    s.shadows = 30;
    s.wb.mode = WhiteBalanceSettings::Mode::Custom;
    s.wb.temperature = 4500;
    s.geometry.rotate90 = 1;
    s.geometry.crop = {0.1, 0.05, 0.8, 0.9};

    const OutputTransform xf(OutputSpace::Srgb, OutputDepth::U8);
    const ImageF proxy = make_proxy(raw.image, 1024);
    const ImageU8 preview = render_preview(raw, proxy, s, xf);
    const ImageU8 exported = encode_u8(render_for_export(raw, s, std::max(preview.width, preview.height)), xf);
    REQUIRE(preview.width == exported.width);
    REQUIRE(preview.height == exported.height);

    // 差が出る理由は 2 つあり、どちらも設計上の性質:
    //  - 縮小フィルタの違い（面積平均 + バイリニア vs バイキュービック + Lanczos3）でエッジがずれる
    //  - プロキシはトーン（非線形）の前に平均し、書き出しはトーンの後に縮小する（5.2 / 5.8 章）ため、
    //    細かい模様のある部分では平均の明るさがわずかに違う
    // ジオメトリや色の誤りなら数十段階ずれるので、4×4 に平均した上で小さい閾値で判定する
    constexpr int k = 4;
    const int w = preview.width / k, h = preview.height / k;
    double abs_sum = 0, signed_sum[3] = {0, 0, 0};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) {
                double d = 0;
                for (int j = 0; j < k; ++j)
                    for (int i = 0; i < k; ++i)
                        d += int(preview.row(y * k + j)[(x * k + i) * 3 + c]) -
                             int(exported.row(y * k + j)[(x * k + i) * 3 + c]);
                d /= k * k;
                abs_sum += std::abs(d);
                signed_sum[c] += d;
            }
    const double n = static_cast<double>(w) * h;
    const double mean = abs_sum / (n * 3);
    INFO("mean |d| " << mean << ", bias R " << signed_sum[0] / n << " G " << signed_sum[1] / n << " B "
                     << signed_sum[2] / n);
    CHECK(mean < 2.0);
    for (double b : signed_sum) CHECK(std::abs(b / n) < 1.5);
}
