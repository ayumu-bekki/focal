# 公開サイト（GitHub Pages）

https://ayumu-bekki.github.io/focal/ に、トップページと使い方ガイドを公開する。

| ページ | 元のファイル |
|---|---|
| トップページ（`/`） | `site/index.html`、`site/style.css`、`site/assets/`（アイコン・スクリーンショット） |
| 使い方ガイド（`/manual/`） | `docs/user-guide.md`（Markdown を HTML にする）、画像は `docs/images/` |

- **作り方**: `site/build.py` が `_site/` を作る（Python の `markdown` パッケージを使う。`pip install markdown`）。
  手元で見るには `python3 site/build.py && python3 -m http.server -d _site`。ガイドの目次のリンクと画像のファイルがあるかも確かめる（足りなければ警告して終了コード 1）。
- **公開**: `.github/workflows/pages.yml` が、`main` で `site/`・`docs/user-guide.md`・`docs/images/` が変わったときに動く。ガイドを直して `main` にマージすれば、サイトも更新される。
- **初回だけ**: リポジトリの Settings ▸ Pages ▸ Build and deployment ▸ Source を **GitHub Actions** にする（`gh api -X POST repos/ayumu-bekki/focal/pages -f build_type=workflow` でもできる）。
- **ダウンロード**: トップページのボタンは、GitHub の Releases（`/releases`）へのリンク。リリースの出し方は `docs/release.md`。
- **スクリーンショットの撮り直し**: トップページの 2 枚（一覧・現像中。ダーク）は `testLandingScreenshots`、ガイドの画像は `testDocScreenshots` ほか。`apps/macos/scripts/ui-test.sh <出力フォルダ> -only-testing:FocalUITests/FocalUITests/testLandingScreenshots` のあと `apps/macos/scripts/make-doc-images.py <出力フォルダ>` で `site/assets/` と `docs/images/` に入る。
- **アイコン**: `site/assets/icon.svg`（元は `apps/macos/Icon/focal_icon.svg`）と、`rsvg-convert` で作った PNG（`icon.png`・`apple-touch-icon.png`・`favicon.png`）。アイコンを変えたら作り直す。
