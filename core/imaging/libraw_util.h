#pragma once

// core 内部専用。LibRaw のインスタンスを扱う共通処理。
#include <libraw/libraw.h>

#include <filesystem>

#include "imaging/raw_decoder.h"

namespace focal {

// open_file のワイド文字対応版。失敗時は Error を投げる
void open_libraw(LibRaw& raw, const std::filesystem::path& path);

// open_file 済みのインスタンスから 7.3 章のメタデータを取り出す
RawMetadata metadata_from_libraw(const LibRaw& raw);

} // namespace focal
