# ADR-10: モジュール構成

- 状態: 確定（design.md v3）

## 決定

`core`（C++20、OS 非依存）/ `capi`（C API）/ `cli` / `apps/macos`（SwiftUI）

## 理由・補足

GUI なしで全画像処理・カタログ処理を検証できるようにする。UI 層には画像処理・カタログのロジックを書かない
