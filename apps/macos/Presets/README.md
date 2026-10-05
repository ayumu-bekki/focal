# Focal 標準の現像プリセット

このフォルダの `*.focalpreset` が、アプリに同梱されます（ビルドで `Focal.app/Contents/Resources/Presets/` に入る）。
アプリの中では「Focal」の見出しの下に出て、利用者は削除・上書きできません。

## 登録の手順

1. Focal の現像画面で調整し、プリセットのメニュー ▸「いまの調整をプリセットとして保存…」で保存する。
2. `~/Library/Application Support/jp.bekki.focal/Presets/` にできた `〜.focalpreset` を、このフォルダへコピーする。
3. `cd apps/macos && xcodegen` のあとビルドし直す。

プリセットは調整だけで、切り取り・回転・傾き補正は含まない（design.md 6.3 章）。
