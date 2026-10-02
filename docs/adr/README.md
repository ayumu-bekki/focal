# ADR 一覧

`design.md` 3 章を ADR ごとに分割したもの。**確定事項**であり、変更が必要なら実装前に人間に確認すること。

| # | 項目 | 決定 |
|---|---|---|
| [ADR-01](adr-01-license.md) | ライセンス | **Apache License 2.0**（v3.18 で確定） |
| [ADR-02](adr-02-metadata.md) | メタデータ | **LibRaw のみ**で取得 |
| [ADR-03](adr-03-interactive-preview.md) | 対話的プレビュー | **縮小プロキシ方式・CPU 実装・float32** |
| [ADR-04](adr-04-pipeline-order.md) | 処理順序 | 5.4 章のパイプラインで確定 |
| [ADR-05](adr-05-display-view.md) | 表示部品 | 現像ビューは **`NSViewRepresentable` でラップした `NSView` + `CALayer`**。core が CPU で作った 8-bit 画像を、色空間タグ付きの `CGImage` として `layer.contents` に設定する |
| [ADR-06](adr-06-color-management.md) | 色管理 | **macOS の表示は OS（ColorSync）に委譲**する。core は表示用に Display P3 の画像を出力し、色空間をタグ付けする。**Little CMS 2（lcms2, MIT）**は書き出しの色変換・ICC 生成と、将来の Windows / Linux の表示変換に使う |
| [ADR-07](adr-07-database.md) | DB | **SQLite 3（C API を薄い RAII ラッパーで直接使用）**、WAL、書き込みは専用スレッド 1 本 |
| [ADR-08](adr-08-undo.md) | Undo | **Undo スタックは core に置く**。写真ごと、**現像・ジオメトリ操作のみ**対象、セッション内のみ（永続化しない）。macOS では **Edit メニューの取り消す / やり直す（⌘Z / ⌘⇧Z）を core の Undo スタックに直接つなぐ**（`UndoManager` には登録しない。テキスト入力中は通常のテキストの Undo に渡す） |
| [ADR-09](adr-09-geometry.md) | ジオメトリ | 出力座標 → ソース座標の**逆写像サンプリング**で実装、座標は正規化値で保存 |
| [ADR-10](adr-10-modules.md) | モジュール構成 | `core`（C++20、OS 非依存）/ `capi`（C API）/ `cli` / `apps/macos`（SwiftUI） |
| [ADR-11](adr-11-build.md) | ビルド | core / capi / cli: **CMake + vcpkg manifest**。macOS アプリ: **Xcode プロジェクト**。core と capi は静的ライブラリ + ヘッダ + modulemap の **XCFramework** にまとめてアプリにリンクする。vcpkg はオーバーレイトリプレットで**LibRaw と libomp（OpenMP ランタイム）を動的リンク**、他は静的リンク。LibRaw は **OpenMP 有効**でビルドし、macOS の libomp は LLVM のソースからビルドする**オーバーレイポート**（`ports/llvm-openmp`）で用意する |
| [ADR-12](adr-12-import.md) | 取り込み | **参照方式を基本**とする。**例外として、SD カードなどからの取り込みだけライブラリのルートの下にコピーする**（v3.19 で変更）。元ファイルは移動・削除・変更しない |
| [ADR-13](adr-13-bridge.md) | ブリッジ | **C API（`extern "C"`、不透明ハンドル）**を core と UI の唯一の境界とする。macOS では Swift ラッパー（`FocalCore` モジュール）で Swift らしい API に包む |
| [ADR-14](adr-14-distribution.md) | 配布 | **Developer ID 署名 + 公証（notarization）、App Sandbox なし**、Hardened Runtime 有効 |
| [ADR-15](adr-15-minimum-os.md) | 最低対応 OS | **macOS 14（Sonoma）** |
