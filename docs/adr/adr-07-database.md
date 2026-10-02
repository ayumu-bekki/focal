# ADR-07: DB

- 状態: 確定（design.md v3）

## 決定

**SQLite 3（C API を薄い RAII ラッパーで直接使用）**、WAL、書き込みは専用スレッド 1 本

## 理由・補足

編集パラメータは JSON + process_version
