#pragma once

#include <filesystem>
#include <functional>

#include "imaging/photo_kind.h"
#include "imaging/raw_decoder.h"
#include "util/image.h"

namespace focal {

// OS の層が持つ画像の読み取り（macOS の ImageIO で HEIF を読む）。core は OS に依存しないので、使える OS が登録する。
// pixels は sRGB・8-bit・向き補正済み。max_long_edge > 0 なら、長辺をそれ以下に縮める（0 は原寸）。meta.flip は 0
struct PlatformImage {
    ImageU8 pixels;
    RawMetadata meta;
};
using PlatformImageReader = std::function<PlatformImage(const std::filesystem::path&, int max_long_edge)>;
void set_platform_image_reader(PlatformImageReader reader);
bool platform_image_reader_available();

// RAW 以外の写真の撮影情報・大きさ（向き補正後）。読めなければ Error を投げる。撮影日時が取れなければ timestamp = 0
RawMetadata read_image_file_metadata(const std::filesystem::path& path, PhotoKind kind);

// RAW 以外の写真を、現像の土台（DecodedRaw）に読み込む。画素は sRGB（埋め込みの ICC は sRGB に変換する）を
// リニアにした 16-bit で、色の行列は恒等（カメラ RGB = リニア sRGB、白バランスは As Shot = 昼光 = 1）。向きは補正しない（meta.flip）。
// これで表示・サムネイル・書き出しは、RAW と同じ経路で動く
DecodedRaw decode_image_file(const std::filesystem::path& path, PhotoKind kind, const DecodeOptions& options = {});

// RAW 以外の写真から一覧用のサムネイルを作る（sRGB、向き補正済み、長辺が long_edge）
ImageU8 image_file_thumbnail(const std::filesystem::path& path, PhotoKind kind, int long_edge);

} // namespace focal
