#pragma once
// LensDatabase の内部（Lensfun の型を使う実装ファイルだけが include する）
#ifdef FOCAL_HAVE_LENSFUN
#include <lensfun/lensfun.h>

#include "imaging/lens_db.h"

namespace focal {

struct LensDatabase::Impl {
    lfDatabase* db = nullptr;
};

// "メーカー|モデル" の lfLens。なければ null（DB が生きている間だけ有効）
const lfLens* lens_by_id(const LensDatabase& db, const std::string& id);
// 写真のカメラとレンズ名から、いちばん近い lfLens。なければ null
const lfLens* lens_by_exif(const LensDatabase& db, const std::string& camera_make, const std::string& camera_model,
                           const std::string& lens_name);

} // namespace focal
#endif
