#!/usr/bin/env bash
# core + capi を XCFramework（FocalCore.xcframework）にまとめる（ADR-11）。
# Xcode のビルド前スクリプトからも呼ばれる。CMake 側は差分ビルドなので、変更がなければ数秒で終わる。
#
# 出力（apps/macos/build/、gitignore 対象）:
#   FocalCore.xcframework   静的ライブラリ（core + capi + 静的リンクの依存）+ ヘッダ + modulemap（module CFocal）
#   Frameworks/             アプリに同梱する dylib（LibRaw、libomp）。ADR-01 により動的リンク
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="$ROOT/apps/macos/build"
PRESET=release
BUILD="$ROOT/build/$PRESET"
LIBDIR="$BUILD/vcpkg_installed/arm64-osx-focal/lib"

# Xcode から呼ばれたときはシェルの環境変数がないので補う
export VCPKG_ROOT="${VCPKG_ROOT:-$HOME/vcpkg}"
export PATH="/opt/local/bin:/usr/local/bin:$PATH"

cd "$ROOT"
if [[ ! -f "$BUILD/build.ninja" ]]; then
  cmake --preset "$PRESET" >/dev/null
fi
cmake --build --preset "$PRESET" --target focal_capi focal_gpu >/dev/null

STATIC_LIBS=(
  "$BUILD/capi/libfocal_capi.a"
  "$BUILD/core/libfocal_core.a"
  "$BUILD/gpu/libfocal_gpu.a"
  "$LIBDIR/liblcms2.a"
  "$LIBDIR/libsqlite3.a"
  "$LIBDIR/libblake3.a"
  "$LIBDIR/libutf8proc.a"
  "$LIBDIR/libjpeg.a"
  "$LIBDIR/libtiff.a"
  "$LIBDIR/libz.a"
)

STAGE="$OUT/stage"
mkdir -p "$STAGE/include/focal" "$OUT/Frameworks"
COMBINED="$STAGE/libFocalCore.a"

# 同梱するライブラリのライセンス文（ADR-01、M6）。アプリの Resources/Licenses に入る
LIC="$OUT/Licenses"
mkdir -p "$LIC"
SHARE="$BUILD/vcpkg_installed/arm64-osx-focal/share"
cp -f "$ROOT/LICENSE" "$LIC/Focal.txt"
for pair in "LibRaw:libraw" "LLVM OpenMP (libomp):llvm-openmp" "Little CMS:lcms" "SQLite:sqlite3" "BLAKE3:blake3" \
            "utf8proc:utf8proc" "libjpeg-turbo:libjpeg-turbo" "LibTIFF:tiff" "zlib:zlib" "nlohmann-json:nlohmann-json" \
            "metal-cpp:metal-cpp"; do
  name="${pair%%:*}"; port="${pair##*:}"
  cp -f "$SHARE/$port/copyright" "$LIC/$name.txt"
done
cat > "$LIC/README.txt" <<'TXT'
Focal は以下のオープンソースソフトウェアを使用しています。各ファイルに著作権表示とライセンス文があります。
Focal uses the following open source software. See each file for the copyright notice and license.

LibRaw（LGPL 2.1 / CDDL 1.0）と LLVM OpenMP（Apache 2.0 with LLVM exception）は、アプリの
Contents/Frameworks に動的ライブラリ（libraw_r.25.0.0.dylib、libomp.dylib）として入っています。
LibRaw は LGPL 2.1 に基づき、利用者が差し替えることができます（差し替えた後はアプリに再署名が必要です）。
LibRaw のソース: https://github.com/LibRaw/LibRaw/tree/0.22.2
LibRaw is dynamically linked under the LGPL 2.1 and can be replaced by the user (re-sign the app afterwards).
TXT

# Xcode から呼ばれたとき: Xcode はこのスクリプトより先に XCFramework の中身（ヘッダと .a）を
# BUILT_PRODUCTS_DIR にコピーするので、作り直した場合はそのコピーも新しくする（古いヘッダで Swift が
# コンパイルされ、API バージョンの不一致やメンバーがないエラーになるのを防ぐ）
sync_built_products() {
  [[ -n "${BUILT_PRODUCTS_DIR:-}" && -d "$BUILT_PRODUCTS_DIR/include" ]] || return 0
  local src="$OUT/FocalCore.xcframework/macos-arm64"
  if ! cmp -s "$src/Headers/focal/focal.h" "$BUILT_PRODUCTS_DIR/include/focal/focal.h"; then
    cp -f "$src/Headers/focal/focal.h" "$BUILT_PRODUCTS_DIR/include/focal/focal.h"
    # 古いヘッダから作ったモジュール（CFocal）のキャッシュも消す
    [[ -n "${OBJROOT:-}" ]] && rm -f "$OBJROOT"/SwiftExplicitPrecompiledModules/CFocal-*.pcm
  fi
  cmp -s "$src/libFocalCore.a" "$BUILT_PRODUCTS_DIR/libFocalCore.a" ||
    cp -f "$src/libFocalCore.a" "$BUILT_PRODUCTS_DIR/libFocalCore.a"
}

# 入力が変わっていなければ作り直さない
STAMP="$STAGE/inputs.sha"
NEW_STAMP=$(shasum "${STATIC_LIBS[@]}" "$ROOT/capi/include/focal/focal.h" | shasum | cut -d' ' -f1)
if [[ -f "$STAMP" && "$(cat "$STAMP")" == "$NEW_STAMP" && -d "$OUT/FocalCore.xcframework" ]]; then
  sync_built_products
  exit 0
fi

libtool -static -no_warning_for_no_symbols -o "$COMBINED" "${STATIC_LIBS[@]}"
cp "$ROOT/capi/include/focal/focal.h" "$STAGE/include/focal/"
cat > "$STAGE/include/module.modulemap" <<'MAP'
module CFocal {
    header "focal/focal.h"
    export *
}
MAP

rm -rf "$OUT/FocalCore.xcframework"
xcodebuild -create-xcframework -library "$COMBINED" -headers "$STAGE/include" \
  -output "$OUT/FocalCore.xcframework" >/dev/null

# 同梱する dylib（install name は @rpath/<名前>）
cp -f "$LIBDIR/libraw_r.25.0.0.dylib" "$LIBDIR/libomp.dylib" "$OUT/Frameworks/"
ln -sf libraw_r.25.0.0.dylib "$OUT/Frameworks/libraw_r.dylib"

echo "$NEW_STAMP" > "$STAMP"
sync_built_products
echo "built $OUT/FocalCore.xcframework"
