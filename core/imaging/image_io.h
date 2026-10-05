#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "util/image.h"

namespace focal {

enum class TiffCompression { None, Deflate };

// 書き出しファイルに入れる撮影情報（v3.19）。カタログが持っている値（LibRaw で読んだもの）をそのまま書く。
// 撮影日時は EXIF にタイムゾーンがないので、カタログと同じローカル時刻。位置情報・シリアル番号は持たない
struct ExifInfo {
    std::string capture_time;  // "YYYY-MM-DDTHH:MM:SS"。不明なら空
    std::string make, model, lens;
    std::optional<int> iso;
    std::optional<double> exposure_time;  // 秒
    std::optional<double> f_number;
    std::optional<double> focal_length;  // mm
    std::optional<int> orientation;      // EXIF の Orientation（1〜8）。読んだときだけ入る（書き出しは常に 1）

    bool empty() const {
        return capture_time.empty() && make.empty() && model.empty() && lens.empty() && !iso && !exposure_time &&
               !f_number && !focal_length;
    }
};

// JPEG の APP1（"Exif\0\0" から）の中身を作る。width / height は書き出す画像の大きさ（向きは補正済みなので Orientation = 1）
std::vector<uint8_t> build_exif_app1(const ExifInfo& info, int width, int height);

// JPEG の EXIF を読む（テスト・確認用。自分が書く範囲の項目だけ）。EXIF がなければ nullopt
std::optional<ExifInfo> read_jpeg_exif(const std::filesystem::path& path);
// TIFF の撮影情報（撮影日時・メーカー・機種）を読む。なければ nullopt
std::optional<ExifInfo> read_tiff_exif(const std::filesystem::path& path);

// 16-bit / 8-bit RGB の TIFF を書く。icc が空でなければ埋め込む。
// exif があれば、撮影日時・メーカー・機種・ソフト名の標準タグを書く（EXIF の IFD までは書かない）。
void write_tiff(const std::filesystem::path& path, const ImageU16& image, const std::vector<uint8_t>& icc,
                TiffCompression compression = TiffCompression::None, const ExifInfo* exif = nullptr);
void write_tiff(const std::filesystem::path& path, const ImageU8& image, const std::vector<uint8_t>& icc,
                TiffCompression compression = TiffCompression::None, const ExifInfo* exif = nullptr);

// 8-bit RGB の JPEG を書く。icc が空でなければ埋め込む。exif が空でなければ EXIF（APP1）を書く。
void write_jpeg(const std::filesystem::path& path, const ImageU8& image, int quality,
                const std::vector<uint8_t>& icc, const ExifInfo* exif = nullptr);

// 読み戻し（テスト・比較用）。8-bit は 16-bit に拡張せず、bits で区別する。
struct LoadedImage {
    int width = 0;
    int height = 0;
    int bits = 0;
    std::vector<uint16_t> data;  // RGB インターリーブ。8-bit の場合も値は 0..255
    std::vector<uint8_t> icc;
};

LoadedImage read_tiff(const std::filesystem::path& path);

// メモリ上の JPEG を RGB 8-bit で読む。min_long_edge > 0 なら、長辺がそれを下回らない範囲で
// libjpeg の縮小デコード（1/2, 1/4, 1/8）を使って速く読む。
ImageU8 decode_jpeg(const uint8_t* data, size_t size, int min_long_edge = 0);
LoadedImage read_jpeg(const std::filesystem::path& path);

// ---- 通常の画像ファイル（JPEG・TIFF・PNG）の読み取り（v3.22、取り込み用）-----------------------------

// ヘッダだけ読んだ結果（画素は読まない）。width / height は向き補正の前の大きさ
struct ImageHeader {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> icc;       // 埋め込みの ICC プロファイル。なければ空（sRGB とみなす）
    std::optional<ExifInfo> exif;   // 撮影情報・向き。なければ nullopt
};

ImageHeader read_jpeg_header(const std::filesystem::path& path);
ImageHeader read_tiff_header(const std::filesystem::path& path);
ImageHeader read_png_header(const std::filesystem::path& path);

// 画素を RGB 8-bit で読む（向きは補正しない。色空間は変換しない）。min_long_edge > 0 の JPEG は縮小デコードで速く読む。
// 16-bit・パレット・グレー・アルファ付きも RGB 8-bit にする（アルファは捨てる）
ImageU8 decode_jpeg_file(const std::filesystem::path& path, int min_long_edge = 0);
ImageU8 decode_tiff_file(const std::filesystem::path& path);
ImageU8 decode_png_file(const std::filesystem::path& path);

// EXIF の Orientation（1〜8）→ LibRaw の flip 値（0 / 3 / 5 / 6 / 1 / 2 / 4 / 7）。範囲外は 0
int flip_from_exif_orientation(int orientation);

// icc（RGB のプロファイル）の色を sRGB に変換する。icc が空・sRGB・RGB でない（グレー・CMYK）ときは何もしない
void convert_to_srgb(ImageU8& image, const std::vector<uint8_t>& icc);

} // namespace focal
