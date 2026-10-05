#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>

#include "util/image.h"
#include "util/matrix.h"

namespace focal {

// RAW から取り出した色情報（5.1 章）。
struct ColorInfo {
    // デモザイク時に実際に適用された As Shot の WB 係数（G = 1 に正規化）
    std::array<double, 3> as_shot_wb{1, 1, 1};
    // カメラの昼光（D65）WB 係数（G = 1 に正規化）
    std::array<double, 3> daylight_wb{1, 1, 1};
    // WB 適用後のカメラ RGB → リニア sRGB（LibRaw の rgb_cam）
    Mat3 rgb_cam = Mat3::identity();
    // XYZ → カメラ RGB（WB 前）。LibRaw の cam_xyz、なければ rgb_cam から導出
    Mat3 cam_xyz = Mat3::identity();
    // RAW 以外の写真（JPEG・TIFF・PNG・HEIF）: すでに表示用の階調。ベースカーブを入れない（v3.22）
    bool display_referred = false;
};

// 7.3 章のメタデータ。
struct RawMetadata {
    std::string make;
    std::string model;
    std::string normalized_make;
    std::string lens;
    float iso = 0;
    float shutter = 0;   // 秒
    float aperture = 0;  // F 値
    float focal_length = 0;
    std::time_t timestamp = 0;
    int width = 0;   // 向き補正後
    int height = 0;  // 向き補正後
    int flip = 0;    // LibRaw の flip 値
};

struct DecodeOptions {
    bool half_size = false;
    // true になったら LibRaw の進捗コールバックで打ち切り、Error(Cancelled) を投げる（先読みの中止用）
    const std::atomic<bool>* cancel = nullptr;
};

struct DecodedRaw {
    // フル解像度バッファ: カメラ RGB（As Shot WB 適用済み）、リニア、向き未適用、0..65535
    ImageU16 image;
    ColorInfo color;
    RawMetadata meta;
    int flip = 0;
};

// LibRaw でデモザイクまで行う。失敗時は Error を投げる。
// 拡張子が RAW 以外の写真（JPEG・TIFF・PNG・HEIF）なら、imaging/photo_file の経路で読む（v3.22）。
DecodedRaw decode_raw(const std::filesystem::path& path, const DecodeOptions& options = {});

// メタデータのみ取得する（unpack しない）。RAW 以外の写真は拡張子で別の経路。
RawMetadata read_raw_metadata(const std::filesystem::path& path);

} // namespace focal
