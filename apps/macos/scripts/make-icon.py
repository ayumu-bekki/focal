#!/usr/bin/env python3
"""アプリのアイコン（Focal/AppIcon.icon、Icon Composer の形式）を Icon/focal_icon.svg から作る。

macOS 26 以降はこの形式の層から、標準・ダーク・クリア・色付きの見た目をシステムが作り分ける
（それより前の macOS 向けの画像は Xcode がビルド時に作る）。
- 背景: SVG の板のグラデーションを塗りにする。ダークでは暗いグレーのグラデーション
- 前景: 3 つの円を 1 つずつ PNG にして層にする（Icon Composer の SVG の層は放射状のグラデーションに対応しない）。
  ダークでは円の不透明度を上げた画像を使う（暗い背景で沈まないように）
- 角丸の形・影・ガラスの質感はシステムが付けるので、SVG の板と影は使わない

使い方: apps/macos/scripts/make-icon.py   （rsvg-convert が要る: sudo port install librsvg）
確認:   "/Applications/Xcode.app/Contents/Applications/Icon Composer.app/Contents/Executables/ictool" \\
          Focal/AppIcon.icon --export-image --output-file out.png --platform macOS --rendition Dark \\
          --width 512 --height 512 --scale 1
"""
import json
import re
import shutil
import subprocess
from pathlib import Path

MAC = Path(__file__).resolve().parent.parent
SRC = MAC / "Icon" / "focal_icon.svg"
OUT = MAC / "Focal" / "AppIcon.icon"

# SVG の板（x, y = 46、幅・高さ = 420）をアイコンの形いっぱいに合わせる
PLATE = "46 46 420 420"
SIZE = 1024
DARK_OPACITY_GAIN = 1.6   # ダークでの円の不透明度の倍率
DARK_OPACITY_MAX = 0.85


def p3(hex_color: str) -> str:
    r, g, b = (int(hex_color[i:i + 2], 16) / 255 for i in (1, 3, 5))
    return f"display-p3:{r:.5f},{g:.5f},{b:.5f},1.00000"


def render(svg_body: str, defs: str, out: Path) -> None:
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{PLATE}" width="{SIZE}" height="{SIZE}">'
           f"<defs>{defs}</defs>{svg_body}</svg>")
    subprocess.run(["rsvg-convert", "-w", str(SIZE), "-h", str(SIZE), "-o", str(out)],
                   input=svg.encode(), check=True)


def main() -> None:
    src = SRC.read_text()
    defs = re.search(r"<defs>(.*)</defs>", src, re.S).group(1)
    defs_dark = re.sub(r'stop-opacity="([0-9.]+)"',
                       lambda m: f'stop-opacity="{min(DARK_OPACITY_MAX, float(m.group(1)) * DARK_OPACITY_GAIN):.3f}"',
                       defs)
    circles = re.findall(r"<circle[^>]*/>", src)

    shutil.rmtree(OUT, ignore_errors=True)
    (OUT / "Assets").mkdir(parents=True)
    layers = []
    for i, c in enumerate(circles, 1):
        render(c, defs, OUT / "Assets" / f"sphere{i}.png")
        render(c, defs_dark, OUT / "Assets" / f"sphere{i}-dark.png")
        # 書き分け（*-specializations）を使うときは、既定の値も書き分けの中に入れる
        # （image-name と両方書くと書き分けが無視される。fill も同じ）
        layers.append({
            "name": f"sphere{i}",
            "glass": False,
            "image-name-specializations": [
                {"value": f"sphere{i}.png"},
                {"appearance": "dark", "value": f"sphere{i}-dark.png"},
            ],
        })
    icon = {
        "fill-specializations": [
            {"value": {"linear-gradient": [p3("#ffffff"), p3("#f0f2f5")]}},
            {"appearance": "dark", "value": {"linear-gradient": [p3("#2c3038"), p3("#14161a")]}},
        ],
        # 層は上から順（SVG では後に書いたものが上）
        "groups": [{
            "layers": list(reversed(layers)),
            "shadow": {"kind": "neutral", "opacity": 0.5},
            "translucency": {"enabled": True, "value": 0.3},
        }],
        "supported-platforms": {"squares": ["macOS"]},
    }
    (OUT / "icon.json").write_text(json.dumps(icon, indent=2) + "\n")
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
