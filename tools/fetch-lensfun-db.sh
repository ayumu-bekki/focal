#!/usr/bin/env bash
# Lensfun のレンズデータベース（CC BY-SA 3.0）と、Lensfun・データのライセンス文を取得する（v3.27、design.md 5.13 章・ADR-01）。
#
#   tools/fetch-lensfun-db.sh [出力フォルダ]     既定: build/lensfun
#
# 出力:
#   <出力>/db/*.xml            レンズデータベース（Lensfun 0.3.x が読む「版 1」。未改変のまま同梱する）
#   <出力>/DB_DATE.txt         データの日付（Lensfun の配布元の timestamp。「サードパーティのライセンス」に載せる）
#   <出力>/licenses/*.txt      LGPL-3.0・GPL-3.0・CC BY-SA 3.0 の文（Lensfun v0.3.4 のタグの docs/ から。チェックサムを確認する）
#
# データは配布元（https://lensfun.github.io/db/）の最新の版 1 で、更新のたびに中身が変わる。再現できるよう、取得した日付を
# DB_DATE.txt に残す。ネットワークがなくても、取得済みのデータがあればそれを使う（--refresh で取り直す）。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/build/lensfun}"
REFRESH=0
[[ "${2:-}" == "--refresh" || "${1:-}" == "--refresh" ]] && { REFRESH=1; [[ "${1:-}" == "--refresh" ]] && OUT="$ROOT/build/lensfun"; }
BASE="https://lensfun.github.io/db"
TAG_RAW="https://raw.githubusercontent.com/lensfun/lensfun/v0.3.4/docs"

if command -v sha256sum >/dev/null 2>&1; then sha256() { sha256sum "$1" | cut -d' ' -f1; }
else sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }; fi

mkdir -p "$OUT/licenses"

# ライセンス文（タグで固定。チェックサムが合うものだけ使う）
fetch_license() {
  local name="$1" sha="$2" file="$OUT/licenses/$1.txt"
  if [[ -f "$file" ]] && [[ "$(sha256 "$file")" == "$sha" ]]; then return; fi
  curl -fsSL --retry 3 -o "$file.part" "$TAG_RAW/$name.txt"
  [[ "$(sha256 "$file.part")" == "$sha" ]] || { echo "checksum mismatch: $name" >&2; rm -f "$file.part"; exit 1; }
  mv "$file.part" "$file"
}
fetch_license lgpl-3.0 a853c2ffec17057872340eee242ae4d96cbf2b520ae27d903e1b2fef1a5f9d1c
fetch_license gpl-3.0 8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903
fetch_license cc-by-sa-3.0 0e15ce5245342be97a03846ed605c2ff76c926719b759262df8c698be2af2bb7

# データベース
if [[ -d "$OUT/db" ]] && [[ -f "$OUT/DB_DATE.txt" ]] && [[ "$REFRESH" == 0 ]]; then
  echo "ok      lensfun db ($(cat "$OUT/DB_DATE.txt"))"; exit 0
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
curl -fsSL --retry 3 -o "$TMP/versions.json" "$BASE/versions.json" || {
  [[ -d "$OUT/db" ]] && { echo "ネットワークに接続できないので、取得済みのレンズデータを使います" >&2; exit 0; }
  echo "レンズデータを取得できません" >&2; exit 1; }
curl -fsSL --retry 3 -o "$TMP/version_1.tar.bz2" "$BASE/version_1.tar.bz2"
bzip2 -t "$TMP/version_1.tar.bz2"
mkdir -p "$TMP/db"
tar -xjf "$TMP/version_1.tar.bz2" -C "$TMP/db"
count=$(ls "$TMP/db"/*.xml 2>/dev/null | wc -l | tr -d ' ')
[[ "$count" -ge 20 ]] || { echo "レンズデータの中身が不正です（XML が $count 個）" >&2; exit 1; }
grep -q '<lensdatabase version="1">' "$TMP/db/mil-nikon.xml" || { echo "レンズデータの版が 1 ではありません" >&2; exit 1; }
# timestamp（秒）→ 日付
stamp=$(python3 -c "import json,sys,datetime;print(datetime.datetime.fromtimestamp(json.load(open('$TMP/versions.json'))[0],datetime.timezone.utc).strftime('%Y-%m-%d'))")
rm -rf "$OUT/db"
mv "$TMP/db" "$OUT/db"
echo "$stamp" > "$OUT/DB_DATE.txt"
echo "fetch   lensfun db $stamp ($count files)"
