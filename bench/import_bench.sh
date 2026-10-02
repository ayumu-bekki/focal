#!/usr/bin/env bash
# 13 章 M1 の完了条件「1,000 枚規模で取り込みが完了する」の計測。
# tests/data の 5 機種を、APFS のクローン（cp -c、ディスク容量をほぼ使わない）で N 枚に増やして取り込む。
# 使い方: bench/import_bench.sh [N=1000] [作業フォルダ]
set -euo pipefail
cd "$(dirname "$0")/.."

N=${1:-1000}
WORK=${2:-$(mktemp -d -t focal-import-bench)}
FOCAL=./build/release/cli/focal
SRC=(tests/data/canon_eos_m50.CR3 tests/data/nikon_z7.NEF tests/data/sony_ilce7m3.ARW tests/data/fujifilm_xt3.RAF tests/data/ricoh_gr3.DNG)

LIB="$WORK/library"
rm -rf "$LIB" "$WORK/catalog.sqlite"* "$WORK/thumbs"
for ((i = 0; i < N; i++)); do
  src=${SRC[$((i % ${#SRC[@]}))]}
  dir="$LIB/$(printf '%04d' $((2018 + i / 250)))/$(printf '%02d' $((1 + (i / 25) % 12)))"
  mkdir -p "$dir"
  cp -c "$src" "$dir/$(printf 'IMG_%05d' "$i").${src##*.}" 2>/dev/null || cp "$src" "$dir/$(printf 'IMG_%05d' "$i").${src##*.}"
done
echo "library: $N files in $LIB"

C=(--catalog "$WORK/catalog.sqlite" --cache "$WORK/thumbs")
echo "== import（サムネイル作成あり、キャッシュなし）"
"$FOCAL" import "$LIB" "${C[@]}" 2>/dev/null
echo "== rescan（変更なし）"
"$FOCAL" import "$LIB" "${C[@]}" 2>/dev/null
echo "== ls --rating 0 の件数"
"$FOCAL" ls "${C[@]}" --limit 0 2>&1 >/dev/null | tail -1
echo "thumbnails: $(find "$WORK/thumbs" -name '*.jpg' | wc -l | tr -d ' ') files, $(du -sh "$WORK/thumbs" | cut -f1)"
echo "work dir: $WORK"
