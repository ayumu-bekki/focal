# ADR-13: ブリッジ

- 状態: 確定（design.md v3）

## 決定

**C API（`extern "C"`、不透明ハンドル）**を core と UI の唯一の境界とする。macOS では Swift ラッパー（`FocalCore` モジュール）で Swift らしい API に包む

## 理由・補足

Swift の C++ 直接相互運用は使わない。C ABI なら将来の Windows（C# など）・Linux（GTK など）の UI からも同じ境界を使える。規約は 4.3 章
