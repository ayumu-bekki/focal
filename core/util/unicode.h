#pragma once

#include <string>
#include <string_view>

namespace focal {

// NFC に正規化する（7.1 章: パスとファイル名は NFC で保存・比較する）。
// 不正な UTF-8 はそのまま返す（Linux では任意のバイト列のファイル名がありうるため）。
std::string to_nfc(std::string_view utf8);

// 大文字小文字を区別しない照合用のキー（NFC + Unicode case folding）。
// macOS / Windows の一般的なファイルシステムは大文字小文字を区別しないため、再スキャンの照合に使う。
std::string casefold_key(std::string_view utf8);

} // namespace focal
