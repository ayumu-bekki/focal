# ADR-05: 表示部品

- 状態: 確定（design.md v3）

## 決定

現像ビューは **`NSViewRepresentable` でラップした `NSView` + `CALayer`**。core が CPU で作った 8-bit 画像を、色空間タグ付きの `CGImage` として `layer.contents` に設定する

## 理由・補足

SwiftUI の `Image` は大きな画像の頻繁な差し替えと 100% 表示の画素対応に向かないため。Metal は v1 では使わない
