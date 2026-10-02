#!/usr/bin/env bash
# 10 万件のグリッドのスクロール性能（13 章 M2 の完了条件、9.1 章のグリッド方式の比較）。
# アプリ自身が決まった速度でスクロールし（FOCAL_SCROLL_BENCH=1）、その間のヒッチを数える。
# 使い方: apps/macos/scripts/scroll-bench.sh [件数=100000] [作業フォルダ]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
N=${1:-100000}
WORK=${2:-"$ROOT/apps/macos/build/dummy$N"}
export PATH="/opt/local/bin:$PATH"

"$ROOT/apps/macos/scripts/build-core.sh"
cmake --build --preset release --target focal >/dev/null
[[ -f "$WORK/catalog.sqlite" ]] || "$ROOT/apps/macos/scripts/make-dummy-library.py" "$N" "$WORK"

cd "$ROOT/apps/macos"
xcodegen -q
TEST_RUNNER_FOCAL_CATALOG="$WORK/catalog.sqlite" TEST_RUNNER_FOCAL_CACHE="$WORK/thumbs" TEST_RUNNER_FOCAL_SCROLL_TEST=1 \
  xcodebuild test -project Focal.xcodeproj -scheme Focal -destination 'platform=macOS' \
    -derivedDataPath build/DerivedData \
    -only-testing:FocalUITests/FocalUITests/testScrollPerformanceCollectionView \
    -only-testing:FocalUITests/FocalUITests/testScrollPerformanceLazyGrid 2>&1 | grep -E "SCROLL\[|error:"
