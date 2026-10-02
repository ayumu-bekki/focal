#include <catch2/catch_test_macros.hpp>

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
