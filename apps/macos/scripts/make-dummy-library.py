#!/usr/bin/env python3
"""10 万件のスクロール確認用ライブラリを作る（13 章 M2 の完了条件）。

tests/data の 5 機種を APFS のクローン（clonefile、容量をほぼ使わない）で N 枚に増やし、
CLI で --no-thumbs 取り込みする。サムネイルはアプリで表示したものだけが作られる。

使い方: apps/macos/scripts/make-dummy-library.py N 出力フォルダ
  → 出力フォルダ/library, 出力フォルダ/catalog.sqlite, 出力フォルダ/thumbs
"""
import ctypes
import os
import subprocess
import sys

root = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
n = int(sys.argv[1])
out = os.path.abspath(sys.argv[2])
srcs = [os.path.join(root, "tests/data", f) for f in
        ("canon_eos_m50.CR3", "nikon_z7.NEF", "sony_ilce7m3.ARW", "fujifilm_xt3.RAF", "ricoh_gr3.DNG")]

libc = ctypes.CDLL("libSystem.dylib", use_errno=True)
clonefile = libc.clonefile
clonefile.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]

lib = os.path.join(out, "library")
for i in range(n):
    d = os.path.join(lib, f"{2000 + i // 5000:04d}", f"{(i // 400) % 12 + 1:02d}", f"{(i // 100) % 4:02d}")
    os.makedirs(d, exist_ok=True)
    src = srcs[i % len(srcs)]
    dst = os.path.join(d, f"IMG_{i:06d}{os.path.splitext(src)[1]}")
    if not os.path.exists(dst) and clonefile(src.encode(), dst.encode(), 0) != 0:
        raise OSError(ctypes.get_errno(), f"clonefile failed: {dst}")
    if i % 10000 == 0:
        print(f"\r{i}/{n}", end="", flush=True)
print(f"\r{n} files in {lib}")

focal = os.path.join(root, "build/release/cli/focal")
subprocess.run([focal, "import", lib, "--catalog", os.path.join(out, "catalog.sqlite"),
                "--cache", os.path.join(out, "thumbs"), "--no-thumbs"], check=True)
