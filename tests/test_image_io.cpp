#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>

#include "imaging/image_io.h"
#include "imaging/output_transform.h"

using namespace focal;
namespace fs = std::filesystem;

namespace {

fs::path temp_path(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / "focal_tests";
    fs::create_directories(dir);
    return dir / name;
}

template <class T>
Image<T> gradient(int w, int h, T max) {
    Image<T> img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            T* p = img.row(y) + x * 3;
            p[0] = static_cast<T>(static_cast<double>(x) / (w - 1) * max);
            p[1] = static_cast<T>(static_cast<double>(y) / (h - 1) * max);
            p[2] = static_cast<T>(max / 2);
        }
    return img;
}

} // namespace

TEST_CASE("16-bit TIFF: ICC 埋め込み・ビット深度・サイズを読み戻しで確認", "[io]") {
    const auto icc = OutputTransform(OutputSpace::Srgb, OutputDepth::U16).icc_profile();
    const ImageU16 img = gradient<uint16_t>(64, 48, 65535);
    const fs::path path = temp_path("rgb16.tif");
    write_tiff(path, img, icc);
    const LoadedImage back = read_tiff(path);
    CHECK(back.width == 64);
    CHECK(back.height == 48);
    CHECK(back.bits == 16);
    CHECK(back.icc == icc);
    CHECK(std::equal(back.data.begin(), back.data.end(), img.data.begin()));
}

TEST_CASE("8-bit TIFF（Deflate）", "[io]") {
    const ImageU8 img = gradient<uint8_t>(33, 17, 255);
    const fs::path path = temp_path("rgb8.tif");
    write_tiff(path, img, {}, TiffCompression::Deflate);
    const LoadedImage back = read_tiff(path);
    CHECK(back.bits == 8);
    CHECK(back.icc.empty());
    CHECK(std::equal(back.data.begin(), back.data.end(), img.data.begin()));
}

TEST_CASE("JPEG: ICC 埋め込みとサイズ", "[io]") {
    const auto icc = OutputTransform(OutputSpace::Srgb, OutputDepth::U8).icc_profile();
    const ImageU8 img = gradient<uint8_t>(80, 60, 255);
    const fs::path path = temp_path("rgb.jpg");
    write_jpeg(path, img, 95, icc);
    const LoadedImage back = read_jpeg(path);
    CHECK(back.width == 80);
    CHECK(back.height == 60);
    CHECK(back.bits == 8);
    CHECK(back.icc == icc);
    int max_diff = 0;
    for (size_t i = 0; i < img.data.size(); ++i)
        max_diff = std::max(max_diff, std::abs(int(back.data[i]) - int(img.data[i])));
    CHECK(max_diff <= 8);
}

TEST_CASE("日本語・NFD のファイル名で書ける", "[io]") {
    // 「が」を NFD（か + 濁点）で
    const fs::path path = temp_path("\xe3\x81\x8b\xe3\x82\x99\xe5\x86\x99\xe7\x9c\x9f.tif");
    write_tiff(path, gradient<uint8_t>(8, 8, 255), {});
    CHECK(read_tiff(path).width == 8);
}

TEST_CASE("EXIF: JPEG に書いて読み戻す（足りない項目は書かない・長い文字列・小数・露出時間）", "[io][exif]") {
    ExifInfo e;
    e.capture_time = "2026-10-04T11:11:57";
    e.make = "Canon";
    e.model = "Canon EOS 6D Mark II";
    e.lens = "EF24-105mm f/4L IS USM";
    e.iso = 800;
    e.exposure_time = 1.0 / 50;
    e.f_number = 4.5;
    e.focal_length = 35.0;
    const auto path = temp_path("exif.jpg");
    write_jpeg(path, gradient<uint8_t>(16, 12, 255), 90, OutputTransform(OutputSpace::Srgb, OutputDepth::U8).icc_profile(), &e);
    // 画像と ICC は壊れない
    const LoadedImage img = read_jpeg(path);
    CHECK(img.width == 16);
    CHECK_FALSE(img.icc.empty());
    const auto back = read_jpeg_exif(path);
    REQUIRE(back.has_value());
    CHECK(back->capture_time == e.capture_time);
    CHECK(back->make == e.make);
    CHECK(back->model == e.model);
    CHECK(back->lens == e.lens);
    CHECK(back->iso == 800);
    CHECK(std::abs(*back->exposure_time - 0.02) < 1e-6);
    CHECK(std::abs(*back->f_number - 4.5) < 1e-6);
    CHECK(std::abs(*back->focal_length - 35.0) < 1e-6);

    // 撮影日時だけ（ほかは不明）でも書ける。EXIF なしで書けば EXIF はない
    ExifInfo only;
    only.capture_time = "2000-01-02T03:04:05";
    write_jpeg(path, gradient<uint8_t>(8, 8, 255), 90, {}, &only);
    const auto b2 = read_jpeg_exif(path);
    REQUIRE(b2.has_value());
    CHECK(b2->capture_time == only.capture_time);
    CHECK(b2->make.empty());
    CHECK_FALSE(b2->iso.has_value());
    write_jpeg(path, gradient<uint8_t>(8, 8, 255), 90, {});
    CHECK_FALSE(read_jpeg_exif(path).has_value());

    // 形が違う日時は書かない（壊れた EXIF にしない）
    ExifInfo bad;
    bad.capture_time = "not a date";
    bad.make = "X";
    write_jpeg(path, gradient<uint8_t>(8, 8, 255), 90, {}, &bad);
    const auto b3 = read_jpeg_exif(path);
    REQUIRE(b3.has_value());
    CHECK(b3->capture_time.empty());
    CHECK(b3->make == "X");

    // APP1 の中身: "Exif\0\0" + リトルエンディアンの TIFF、サイズは 64KB 未満
    const auto app1 = build_exif_app1(e, 16, 12);
    CHECK(app1.size() < 65000);
    CHECK(std::string(app1.begin(), app1.begin() + 4) == "Exif");
    CHECK(app1[6] == 'I');
    CHECK(app1[7] == 'I');
}
