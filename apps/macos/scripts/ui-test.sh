#!/usr/bin/env bash
# GUI スモークテスト（12 章）。CLI でテスト用カタログを作り、XCUITest を実行してスクリーンショットを保存する。
# 使い方: apps/macos/scripts/ui-test.sh [出力フォルダ]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="$(mkdir -p "${1:-$ROOT/apps/macos/build/ui-test}" && cd "${1:-$ROOT/apps/macos/build/ui-test}" && pwd)"
WORK="$(mktemp -d -t focal-ui)"
export PATH="/opt/local/bin:$PATH"

"$ROOT/apps/macos/scripts/build-core.sh"
cmake --build --preset release --target focal >/dev/null
mkdir -p "$WORK/lib" "$OUT"
for f in "$ROOT"/tests/data/*.{CR3,NEF,ARW,RAF,DNG}; do cp -c "$f" "$WORK/lib/"; done
"$ROOT/build/release/cli/focal" import "$WORK/lib" --catalog "$WORK/catalog.sqlite" --cache "$WORK/thumbs" >/dev/null

# 書き出しのテスト用のフォルダと、色の比較の基準（ニコンのカラーチェッカーの写真を CLI で書き出したもの）
FOCAL="$ROOT/build/release/cli/focal"
C=(--catalog "$WORK/catalog.sqlite" --cache "$WORK/thumbs")
mkdir -p "$WORK/export" "$WORK/ref"
COLOR_INDEX=$("$FOCAL" ls "${C[@]}" 2>/dev/null | grep -n "nikon_z7" | cut -d: -f1)
NIKON_ID=$("$FOCAL" ls "${C[@]}" 2>/dev/null | grep "nikon_z7" | awk '{print $1}')
"$FOCAL" export "$NIKON_ID" --dest "$WORK/ref" --long-edge 1024 "${C[@]}" >/dev/null
COLOR_REF=$("$FOCAL" colorgrid "$WORK/ref/nikon_z7.jpg" --cols 48 --rows 32 --inset 0.01)
COLOR_ASPECT=$(sips -g pixelWidth -g pixelHeight "$WORK/ref/nikon_z7.jpg" | awk '/pixelWidth/{w=$2} /pixelHeight/{h=$2} END{print w/h}')

cd "$ROOT/apps/macos"
xcodegen -q
TEST_RUNNER_FOCAL_CATALOG="$WORK/catalog.sqlite" \
TEST_RUNNER_FOCAL_CACHE="$WORK/thumbs" \
TEST_RUNNER_FOCAL_EXPORT_DIR="$WORK/export" \
TEST_RUNNER_FOCAL_COLOR_INDEX="$COLOR_INDEX" \
TEST_RUNNER_FOCAL_COLOR_REF="$COLOR_REF" \
TEST_RUNNER_FOCAL_COLOR_ASPECT="$COLOR_ASPECT" \
TEST_RUNNER_FOCAL_FIRST_RUN_HOME="$WORK/first-run" \
TEST_RUNNER_FOCAL_GPU="${FOCAL_GPU:-1}" \
TEST_RUNNER_FOCAL_WINDOW_SCREEN="${FOCAL_WINDOW_SCREEN:-0}" \
  xcodebuild test -project Focal.xcodeproj -scheme Focal -destination 'platform=macOS' \
    -derivedDataPath "$ROOT/apps/macos/build/DerivedData" -resultBundlePath "$WORK/result.xcresult" "${@:2}" \
  | grep -E "error:|Test Case|TEST (SUCCEEDED|FAILED)|Executed" || true
# テストランナーはサンドボックス内で動くので、スクリーンショットは結果バンドルから書き出す
xcrun xcresulttool export attachments --path "$WORK/result.xcresult" --output-path "$OUT" >/dev/null
echo "screenshots: $OUT"
rm -rf "$WORK"
