#!/usr/bin/env python3
"""公開サイト（GitHub Pages）を _site/ に作る。

  python3 site/build.py            # → _site/
  python3 -m http.server -d _site  # 手元で確認する

- トップページ: site/index.html + site/style.css + site/assets/（アイコン・スクリーンショット）
- 使い方ガイド: docs/user-guide.md を HTML にして _site/manual/ に置く（画像は docs/images/）
Markdown の変換に Python の markdown パッケージを使う（pip install markdown）。
"""
import html
import pathlib
import re
import shutil
import sys

import markdown

ROOT = pathlib.Path(__file__).resolve().parents[1]
SITE = ROOT / "site"
OUT = ROOT / "_site"
GUIDE = ROOT / "docs" / "user-guide.md"
IMAGES = ROOT / "docs" / "images"


def slugify(value, separator):
    """GitHub の見出しの id と同じ作り方（ガイドの目次のリンクがそのまま使える）。日本語を残す"""
    value = re.sub(r"[^\w\- ]", "", value.lower(), flags=re.UNICODE)
    return value.strip().replace(" ", separator)


NAV = """<header class="bar">
  <div class="wrap">
    <a class="brand" href="../"><img src="../assets/icon.png" alt="">Focal</a>
    <nav>
      <a href="../#features">特長</a>
      <a href="../#capabilities">できること</a>
      <a href="./">使い方ガイド</a>
      <a href="https://github.com/ayumu-bekki/focal">GitHub</a>
    </nav>
  </div>
</header>"""

PAGE = """<!doctype html>
<html lang="ja">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title} — Focal</title>
<meta name="description" content="Focal（macOS 用の RAW 写真管理・現像アプリ）の使い方ガイド。">
<link rel="icon" type="image/png" href="../assets/favicon.png">
<link rel="apple-touch-icon" href="../assets/apple-touch-icon.png">
<link rel="stylesheet" href="../style.css">
</head>
<body>
{nav}
<div class="wrap manual">
  <aside class="toc" aria-label="目次">
{toc}
  </aside>
  <article>
{body}
  </article>
</div>
<footer>
  <div class="wrap">
    <p>このガイドは <a href="https://github.com/ayumu-bekki/focal/blob/main/docs/user-guide.md">GitHub 上の Markdown</a> から作っています。誤りや分かりにくいところは <a href="https://github.com/ayumu-bekki/focal/issues">Issue</a> で教えてください。</p>
  </div>
</footer>
</body>
</html>
"""


def toc_html(tokens, depth=0):
    """見出しの木（h2・h3）をサイドバーの入れ子のリストにする"""
    items = []
    for t in tokens:
        if t["level"] > 3:
            continue
        children = toc_html(t["children"], depth + 1) if t["children"] else ""
        items.append(f'<li><a href="#{html.escape(t["id"])}">{html.escape(t["name"])}</a>{children}</li>')
    return "<ul>" + "".join(items) + "</ul>" if items else ""


def build_manual():
    text = GUIDE.read_text(encoding="utf-8")
    md = markdown.Markdown(
        extensions=["tables", "fenced_code", "toc", "sane_lists"],
        extension_configs={"toc": {"slugify": slugify, "toc_depth": "2-3"}},
    )
    body = md.convert(text)
    title = md.toc_tokens[0]["name"] if md.toc_tokens else "使い方ガイド"
    # h1（ガイドの題名）の下の h2 以下を目次にする
    tokens = md.toc_tokens[0]["children"] if md.toc_tokens and md.toc_tokens[0]["level"] == 1 else md.toc_tokens
    page = PAGE.format(title=html.escape(title), nav=NAV, toc=toc_html(tokens), body=body)

    out = OUT / "manual"
    out.mkdir(parents=True, exist_ok=True)
    (out / "index.html").write_text(page, encoding="utf-8")
    shutil.copytree(IMAGES, out / "images", dirs_exist_ok=True)

    # 確認: ページ内のリンク（#…）の行き先があるか、画像のファイルがあるか
    ids = set(re.findall(r'id="([^"]+)"', body))
    problems = 0
    for href in re.findall(r'href="#([^"]+)"', body):
        if href not in ids:
            print(f"  警告: リンク先の見出しがない: #{href}")
            problems += 1
    for src in re.findall(r'<img[^>]+src="([^"]+)"', body):
        if not src.startswith("http") and not (out / src).exists():
            print(f"  警告: 画像がない: {src}")
            problems += 1
    print(f"manual/index.html: 見出し {len(ids)}、画像 {len(re.findall('<img', body))}、警告 {problems}")
    return problems


def main():
    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir()
    for name in ("index.html", "style.css"):
        shutil.copy(SITE / name, OUT / name)
    shutil.copytree(SITE / "assets", OUT / "assets")
    (OUT / ".nojekyll").write_text("", encoding="utf-8")  # Jekyll で処理させない
    problems = build_manual()
    print("→", OUT)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
