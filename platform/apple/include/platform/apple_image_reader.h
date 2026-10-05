#pragma once

namespace focal::platform {

// macOS の ImageIO で HEIF / HEIC を読む部品を、core に登録する（何度呼んでもよい）。
// 画素は sRGB・8-bit・向き補正済み（ディスプレイ P3 などの写真は ImageIO が sRGB へ変換する）
void register_apple_image_reader();

} // namespace focal::platform
