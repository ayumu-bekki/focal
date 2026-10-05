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

# カード取り込みのテスト用: カード（DCIM 付きのフォルダ）、読み込み先、取り込み用の空のカタログ
mkdir -p "$WORK/card/DCIM/100TEST" "$WORK/import-dest" "$WORK/delete-dest" "$WORK/trash"
cp -c "$ROOT/tests/data/canon_eos_m50.CR3" "$WORK/card/DCIM/100TEST/IMG_9001.CR3"

# 現像プリセットのテスト用: カード、読み込み先、同梱（Focal 標準）と利用者のプリセットのフォルダ
mkdir -p "$WORK/preset-card/DCIM/100TEST" "$WORK/preset-dest" "$WORK/builtin-presets" "$WORK/presets"
cp -c "$ROOT/tests/data/canon_eos_m50.CR3" "$WORK/preset-card/DCIM/100TEST/IMG_9101.CR3"
cat > "$WORK/builtin-presets/Focal Standard.focalpreset" <<'JSON'
{"focalPreset": 1, "name": "Focal Standard", "settings": {"schema": 1, "processVersion": 1, "exposure": 1.5, "contrast": 10}}
JSON

# RAW 以外の写真（v3.22）のテスト用: RAW + 同じ名前の JPEG（1 枚）、PNG だけ、JPEG だけ
mkdir -p "$WORK/mixed"
cp -c "$ROOT/tests/data/canon_eos_m50.CR3" "$WORK/mixed/MIX_1.CR3"
sips -s format jpeg "$WORK/mixed/MIX_1.CR3" --out "$WORK/mixed/MIX_1.JPG" >/dev/null
sips -s format png "$WORK/mixed/MIX_1.JPG" --out "$WORK/mixed/MIX_2.png" >/dev/null
cp -c "$WORK/mixed/MIX_1.JPG" "$WORK/mixed/MIX_3.jpg"
"$ROOT/build/release/cli/focal" import "$WORK/mixed" --catalog "$WORK/mixed-catalog.sqlite" --cache "$WORK/thumbs" >/dev/null

# カタログ情報・最適化のテスト用: ★のついた写真が 1 枚あるカタログ
mkdir -p "$WORK/detach"
cp -c "$ROOT/tests/data/canon_eos_m50.CR3" "$WORK/detach/IMG_D1.CR3"
"$ROOT/build/release/cli/focal" import "$WORK/detach" --catalog "$WORK/detach-catalog.sqlite" --cache "$WORK/thumbs" >/dev/null
"$ROOT/build/release/cli/focal" rate 3 1 --catalog "$WORK/detach-catalog.sqlite" >/dev/null

# 読み込み先を外したときの移行のテスト用: 3 つのルート（a・b・c）を持つカタログ
for r in a b c; do
  mkdir -p "$WORK/multi/$r"
  cp -c "$ROOT/tests/data/canon_eos_m50.CR3" "$WORK/multi/$r/IMG_$r.CR3"
  "$ROOT/build/release/cli/focal" import "$WORK/multi/$r" --catalog "$WORK/multi-catalog.sqlite" --cache "$WORK/thumbs" >/dev/null
done

# 重なったルート（以前の二重登録の跡）のテスト用: nested の中の sub を、別のルートとしても持つカタログ
mkdir -p "$WORK/nested/sub"
cp -c "$ROOT/tests/data/canon_eos_m50.CR3" "$WORK/nested/sub/IMG_nested.CR3"
"$ROOT/build/release/cli/focal" import "$WORK/nested" --catalog "$WORK/nested-catalog.sqlite" --cache "$WORK/thumbs" >/dev/null
sqlite3 "$WORK/nested-catalog.sqlite" "
  INSERT INTO roots (path) VALUES ('$WORK/nested/sub');
  INSERT INTO folders (root_id, parent_id, rel_path) VALUES (2, NULL, '');
  INSERT INTO photos (folder_id, file_name, file_size, file_mtime, quick_hash, status, capture_time, rating, flag)
    SELECT (SELECT id FROM folders WHERE root_id = 2), file_name, file_size, file_mtime, quick_hash, 0, capture_time, 3, 0 FROM photos;"

cd "$ROOT/apps/macos"
xcodegen -q
TEST_RUNNER_FOCAL_CATALOG="$WORK/catalog.sqlite" \
TEST_RUNNER_FOCAL_CACHE="$WORK/thumbs" \
TEST_RUNNER_FOCAL_EXPORT_DIR="$WORK/export" \
TEST_RUNNER_FOCAL_IMPORT_SOURCE="$WORK/card" \
TEST_RUNNER_FOCAL_IMPORT_DEST="$WORK/import-dest" \
TEST_RUNNER_FOCAL_IMPORT_CATALOG="$WORK/import-catalog.sqlite" \
TEST_RUNNER_FOCAL_DELETE_CATALOG="$WORK/delete-catalog.sqlite" \
TEST_RUNNER_FOCAL_MULTI_CATALOG="$WORK/multi-catalog.sqlite" \
TEST_RUNNER_FOCAL_NESTED_CATALOG="$WORK/nested-catalog.sqlite" \
TEST_RUNNER_FOCAL_MIXED_CATALOG="$WORK/mixed-catalog.sqlite" \
TEST_RUNNER_FOCAL_DETACH_CATALOG="$WORK/detach-catalog.sqlite" \
TEST_RUNNER_FOCAL_PRESET_CARD="$WORK/preset-card" \
TEST_RUNNER_FOCAL_PRESET_DEST="$WORK/preset-dest" \
TEST_RUNNER_FOCAL_PRESET_CATALOG="$WORK/preset-catalog.sqlite" \
TEST_RUNNER_FOCAL_PRESETS="$WORK/presets" \
TEST_RUNNER_FOCAL_BUILTIN_PRESETS="$WORK/builtin-presets" \
TEST_RUNNER_FOCAL_MULTI_DIR="$WORK/multi" \
TEST_RUNNER_FOCAL_DELETE_DEST="$WORK/delete-dest" \
TEST_RUNNER_FOCAL_DELETE_TRASH="$WORK/trash" \
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
