# ADR-06: 色管理

- 状態: 確定（design.md v3）

## 決定

**macOS の表示は OS（ColorSync）に委譲**する。core は表示用に Display P3 の画像を出力し、色空間をタグ付けする。**Little CMS 2（lcms2, MIT）**は書き出しの色変換・ICC 生成と、将来の Windows / Linux の表示変換に使う

## 理由・補足

ディスプレイプロファイルの取得や画面移動の追従が不要になり、二重変換の問題も構造上起きない
