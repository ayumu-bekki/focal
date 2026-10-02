#!/usr/bin/env bash
# テスト用 RAW を raw.pixls.us から取得する（すべて CC0）。
# 使い方: tests/data/fetch.sh   （取得済みでチェックサムが一致するものはスキップ）
set -euo pipefail
cd "$(dirname "$0")"

# SHA-256 の計算コマンド（macOS は shasum、Linux と Windows の Git Bash は sha256sum）
if command -v sha256sum >/dev/null 2>&1; then
  sha256() { sha256sum "$1" | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
  sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
  sha256() { openssl dgst -sha256 "$1" | awk '{print $NF}'; }
fi

fetch() {
  local name="$1" url="$2" sha="$3"
  if [[ -f "$name" ]] && [[ "$(sha256 "$name")" == "$sha" ]]; then
    echo "ok      $name"; return
  fi
  echo "fetch   $name"
  curl -fsSL --retry 3 -o "$name.part" "$url"
  [[ "$(sha256 "$name.part")" == "$sha" ]] || { echo "checksum mismatch: $name" >&2; rm -f "$name.part"; exit 1; }
  mv "$name.part" "$name"
}

fetch canon_eos_m50.CR3   "https://raw.pixls.us/getfile.php/2663/nice/Canon%20-%20EOS%20M50%20-%20CRAW%20(3:2).CR3" \
  15384b775867ec4c42b11882837f1e368cedc0561832ffab271221e6bb80be4c
fetch nikon_z7.NEF        "https://raw.pixls.us/getfile.php/2792/nice/Nikon%20-%20Z%207%20-%2012bit%2012bit%20compressed%20(3:2).NEF" \
  c861546fbcf5826d5eb26d223e2143fa32968e89f7edbc5bb10c283ea5461ded
fetch sony_ilce7m3.ARW    "https://raw.pixls.us/getfile.php/2414/nice/Sony%20-%20ILCE-7M3%20-%2014bit%2014bit%20compressed%20(3:2).ARW" \
  250784580ea527442c09004417bb0eead484f2bf3ee8f9121a776ac65bb50d0f
fetch fujifilm_xt3.RAF    "https://raw.pixls.us/getfile.php/2783/nice/Fujifilm%20-%20X-T3%20-%2014bit%2014bit%20compressed%20(3:2).RAF" \
  95b33021160b239ceb1a09d46a1b29cb60cb7dd47581feb126b337c219091754
fetch ricoh_gr3.DNG       "https://raw.pixls.us/getfile.php/3115/nice/Ricoh%20-%20GR%20III%20-%2014bit%20(3:2).DNG" \
  05513ee72f7cac4165534c9f64995412084f414a033c80c473c79fd94154292e
