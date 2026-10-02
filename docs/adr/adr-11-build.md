# ADR-11: ビルド

- 状態: 確定（design.md v3.1 で変更）

## 決定

core / capi / cli: **CMake + vcpkg manifest**。macOS アプリ: **Xcode プロジェクト**。core と capi は静的ライブラリ + ヘッダ + modulemap の **XCFramework** にまとめてアプリにリンクする。vcpkg はオーバーレイトリプレットで**LibRaw と libomp（OpenMP ランタイム）を動的リンク**、他は静的リンク。LibRaw は **OpenMP 有効**でビルドし、macOS の libomp は LLVM のソースからビルドする**オーバーレイポート**（`ports/llvm-openmp`）で用意する

## 理由・補足

ADR-01 の動的リンク要件を満たしつつ、配布物の dylib を最小にする。OpenMP で LibRaw の展開・デモザイクが 2〜6 倍速くなる（M0 実測、X-Trans 14.5s → 2.3s）。Apple clang は OpenMP ランタイムを同梱せず、vcpkg にも libomp のポートがないため自前で用意する。libomp は Apache-2.0 WITH LLVM-exception（v3.1 で変更）
