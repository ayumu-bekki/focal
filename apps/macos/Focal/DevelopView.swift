import AppKit
import FocalCore
import SwiftUI

/// 現像ビュー（ADR-05）: NSView + CALayer に、色空間タグ付きの CGImage をそのまま渡す。
/// ディスプレイへの色変換は OS（ColorSync）が行う（ADR-06）。
struct DevelopView: NSViewRepresentable {
    let model: DevelopModel

    func makeNSView(context: Context) -> DevelopNSView {
        let v = DevelopNSView()
        v.model = model
        v.setAccessibilityElement(true)
        v.setAccessibilityIdentifier("developView")
        v.setAccessibilityRole(.image)
        return v
    }

    func updateNSView(_ v: DevelopNSView, context: Context) {
        // 観測している値に触れて、変化したら更新されるようにする
        _ = (model.fitImage, model.region?.x, model.zoom, model.info?.outputWidth, model.cropMode, model.settings,
             model.levelTool)
        v.refresh()
    }
}

final class DevelopNSView: NSView {
    weak var model: DevelopModel?
    private let fitLayer = CALayer()
    private let regionLayer = CALayer()
    private var dragLast: NSPoint?

    // クロップモードの重ね描き（5.6 章: 枠・ハンドル・三分割線）
    private let shadeLayer = CAShapeLayer()
    private let frameLayer = CAShapeLayer()
    private let levelLayer = CAShapeLayer()
    private var cropDrag: (handle: CropTool.Handle, start: DevelopSettings, from: NSPoint)?
    private var levelLine: (from: NSPoint, to: NSPoint)?

    override var isFlipped: Bool { true }

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        // 18% グレー（反射率 18%、L* ≈ 50）。周りの明るさに引きずられずに写真の明るさを判断できる
        layer?.backgroundColor = CGColor(srgbRed: 119 / 255, green: 119 / 255, blue: 119 / 255, alpha: 1)
        layer?.masksToBounds = true  // 100% 表示の画像をビューの外（下部バーやサイドバーの裏）に描かない
        for l in [fitLayer, regionLayer] {
            l.contentsGravity = .resize
            l.actions = ["contents": NSNull(), "bounds": NSNull(), "position": NSNull()]  // 暗黙のアニメーションを止める
            layer?.addSublayer(l)
        }
        regionLayer.magnificationFilter = .nearest
        shadeLayer.fillRule = .evenOdd
        shadeLayer.fillColor = NSColor.black.withAlphaComponent(0.55).cgColor
        frameLayer.fillColor = nil
        frameLayer.strokeColor = NSColor.white.withAlphaComponent(0.9).cgColor
        frameLayer.lineWidth = 1
        levelLayer.strokeColor = NSColor.systemYellow.cgColor
        levelLayer.lineWidth = 2
        levelLayer.lineDashPattern = [6, 4]
        for l in [shadeLayer, frameLayer, levelLayer] {
            l.actions = ["path": NSNull(), "hidden": NSNull()]
            layer?.addSublayer(l)
        }
    }

    required init?(coder: NSCoder) { fatalError() }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        reportSize()
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        reportSize()
        refresh()
    }

    private func reportSize() {
        let scale = window?.backingScaleFactor ?? 2
        model?.setViewSize(pixels: CGSize(width: bounds.width * scale, height: bounds.height * scale),
                           backingScale: scale)
    }

    /// 出力画像の座標 → ビューの座標の変換（ポイント / 出力画素、出力画像の左上の位置）
    private func mapping() -> (pointsPerPixel: CGFloat, origin: CGPoint)? {
        guard let model else { return nil }
        let size = model.outputSize
        guard size.width > 0 else { return nil }
        switch model.zoom {
        case .fit:
            let s = min(bounds.width / size.width, bounds.height / size.height)
            return (s, CGPoint(x: (bounds.width - size.width * s) / 2, y: (bounds.height - size.height * s) / 2))
        case .actual(let c):
            let s = 1 / (window?.backingScaleFactor ?? 2)  // 1 出力画素 = 1 デバイス画素
            return (s, CGPoint(x: bounds.midX - c.x * s, y: bounds.midY - c.y * s))
        }
    }

    func refresh() {
        guard let model else { return }
        window?.invalidateCursorRects(for: self)
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        fitLayer.contents = model.fitImage
        if let m = mapping() {
            let size = model.outputSize
            fitLayer.frame = CGRect(x: m.origin.x, y: m.origin.y, width: size.width * m.pointsPerPixel,
                                    height: size.height * m.pointsPerPixel)
            if case .actual = model.zoom, let r = model.region {
                regionLayer.isHidden = false
                regionLayer.contents = r.image
                regionLayer.frame = CGRect(x: m.origin.x + r.x * m.pointsPerPixel, y: m.origin.y + r.y * m.pointsPerPixel,
                                           width: CGFloat(r.image.width) * m.pointsPerPixel,
                                           height: CGFloat(r.image.height) * m.pointsPerPixel)
            } else {
                regionLayer.isHidden = true
            }
            updateCropOverlay(mapping: m)
        } else if let img = model.fitImage {
            // まだ大きさが分からない（プレビューだけ）: 縦横比を保って中央に
            let s = min(bounds.width / CGFloat(img.width), bounds.height / CGFloat(img.height))
            let w = CGFloat(img.width) * s, h = CGFloat(img.height) * s
            fitLayer.frame = CGRect(x: (bounds.width - w) / 2, y: (bounds.height - h) / 2, width: w, height: h)
            regionLayer.isHidden = true
        }
        CATransaction.commit()
    }

    /// クロップ枠のビュー上の矩形
    private func cropRectInView(_ m: (pointsPerPixel: CGFloat, origin: CGPoint)) -> CGRect? {
        guard let model, model.cropMode else { return nil }
        let size = model.canvasSize
        let s = model.settings
        return CGRect(x: m.origin.x + s.cropX * size.width * m.pointsPerPixel,
                      y: m.origin.y + s.cropY * size.height * m.pointsPerPixel,
                      width: s.cropW * size.width * m.pointsPerPixel, height: s.cropH * size.height * m.pointsPerPixel)
    }

    private func updateCropOverlay(mapping m: (pointsPerPixel: CGFloat, origin: CGPoint)) {
        guard let r = cropRectInView(m) else {
            shadeLayer.isHidden = true
            frameLayer.isHidden = true
            levelLayer.isHidden = true
            return
        }
        shadeLayer.isHidden = false
        frameLayer.isHidden = false
        let shade = CGMutablePath()
        shade.addRect(bounds)
        shade.addRect(r)
        shadeLayer.path = shade

        let path = CGMutablePath()
        path.addRect(r)
        for i in 1...2 {  // 三分割線
            let fx = r.minX + r.width * CGFloat(i) / 3, fy = r.minY + r.height * CGFloat(i) / 3
            path.move(to: CGPoint(x: fx, y: r.minY))
            path.addLine(to: CGPoint(x: fx, y: r.maxY))
            path.move(to: CGPoint(x: r.minX, y: fy))
            path.addLine(to: CGPoint(x: r.maxX, y: fy))
        }
        let h: CGFloat = 14
        for (x, y) in [(r.minX, r.minY), (r.maxX, r.minY), (r.minX, r.maxY), (r.maxX, r.maxY)] {
            let dx: CGFloat = x == r.minX ? 1 : -1, dy: CGFloat = y == r.minY ? 1 : -1
            path.move(to: CGPoint(x: x, y: y + dy * h))
            path.addLine(to: CGPoint(x: x, y: y))
            path.addLine(to: CGPoint(x: x + dx * h, y: y))
        }
        frameLayer.path = path
        frameLayer.lineWidth = 1

        if let line = levelLine {
            let p = CGMutablePath()
            p.move(to: line.from)
            p.addLine(to: line.to)
            levelLayer.path = p
            levelLayer.isHidden = false
        } else {
            levelLayer.isHidden = true
        }
    }

    /// 押した位置のハンドル（角 → 辺 → 中）
    private func hitHandle(_ p: NSPoint, _ r: CGRect) -> CropTool.Handle? {
        let corner: CGFloat = 14, edge: CGFloat = 8
        func near(_ a: CGFloat, _ b: CGFloat, _ d: CGFloat) -> Bool { abs(a - b) <= d }
        let inX = p.x >= r.minX - edge && p.x <= r.maxX + edge, inY = p.y >= r.minY - edge && p.y <= r.maxY + edge
        if near(p.x, r.minX, corner) && near(p.y, r.minY, corner) { return .topLeft }
        if near(p.x, r.maxX, corner) && near(p.y, r.minY, corner) { return .topRight }
        if near(p.x, r.minX, corner) && near(p.y, r.maxY, corner) { return .bottomLeft }
        if near(p.x, r.maxX, corner) && near(p.y, r.maxY, corner) { return .bottomRight }
        if near(p.x, r.minX, edge) && inY { return .left }
        if near(p.x, r.maxX, edge) && inY { return .right }
        if near(p.y, r.minY, edge) && inX { return .top }
        if near(p.y, r.maxY, edge) && inX { return .bottom }
        if r.contains(p) { return .move }
        return nil
    }

    /// ポインタの位置（出力画像の座標）。ビューの外なら nil
    func pointerInOutput() -> CGPoint? {
        guard let window, let m = mapping() else { return nil }
        let p = convert(window.mouseLocationOutsideOfEventStream, from: nil)
        guard bounds.contains(p) else { return nil }
        let size = model?.outputSize ?? .zero
        let x = (p.x - m.origin.x) / m.pointsPerPixel, y = (p.y - m.origin.y) / m.pointsPerPixel
        guard x >= 0, y >= 0, x <= size.width, y <= size.height else { return nil }
        return CGPoint(x: x, y: y)
    }

    // MARK: マウス

    override func mouseDown(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        dragLast = p
        if let model, model.cropMode {
            if model.levelTool {
                levelLine = (p, p)
            } else if let m = mapping(), let r = cropRectInView(m), let h = hitHandle(p, r) {
                cropDrag = (h, model.settings, p)
            }
            return
        }
        if event.clickCount == 2 { model?.toggleZoom(at: pointerInOutput()) }
    }

    override func mouseDragged(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        if let model, model.cropMode {
            if levelLine != nil {
                levelLine?.to = p
                refresh()
            } else if let d = cropDrag, let m = mapping() {
                let size = model.canvasSize
                let dx = (p.x - d.from.x) / (size.width * m.pointsPerPixel)
                let dy = (p.y - d.from.y) / (size.height * m.pointsPerPixel)
                model.dragCrop(from: d.start, handle: d.handle, dx: dx, dy: dy)
            }
            return
        }
        guard let model, case .actual = model.zoom, let last = dragLast else { return }
        let scale = window?.backingScaleFactor ?? 2
        model.pan(by: CGSize(width: (p.x - last.x) * scale, height: (p.y - last.y) * scale))
        dragLast = p
        refresh()
    }

    override func mouseUp(with event: NSEvent) {
        dragLast = nil
        cropDrag = nil
        if let line = levelLine, let model, let m = mapping() {
            levelLine = nil
            // ビューの座標 → キャンバスの画素
            func canvas(_ p: NSPoint) -> CGPoint {
                CGPoint(x: (p.x - m.origin.x) / m.pointsPerPixel, y: (p.y - m.origin.y) / m.pointsPerPixel)
            }
            if hypot(line.to.x - line.from.x, line.to.y - line.from.y) > 10 {
                model.level(from: canvas(line.from), to: canvas(line.to))
            }
            refresh()
        }
    }

    override func resetCursorRects() {
        if model?.cropMode == true {
            addCursorRect(bounds, cursor: model?.levelTool == true ? .crosshair : .arrow)
        } else if case .actual = model?.zoom {
            addCursorRect(bounds, cursor: .openHand)
        }
    }
}

// MARK: - スライダー（9.1 章: ダブルクリックで既定値に戻す）

struct DevSlider: NSViewRepresentable {
    @Binding var value: Double
    let range: ClosedRange<Double>
    let defaultValue: Double
    let identifier: String
    var onBegin: () -> Void = {}
    var onEnd: () -> Void = {}
    /// ダブルクリックの動作。nil なら既定値に戻す
    var onReset: (() -> Void)? = nil

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    /// 大きさは SwiftUI 側で決める（幅は提案どおり、高さは小さいスライダーの高さ）。
    /// AppKit に大きさを問い合わせると、外観の解決などでレイアウトのたびに重くなる
    func sizeThatFits(_ proposal: ProposedViewSize, nsView: ResettableSlider, context: Context) -> CGSize? {
        CGSize(width: proposal.width ?? 120, height: Self.height)
    }

    static let height: CGFloat = 16

    func makeNSView(context: Context) -> ResettableSlider {
        let s = ResettableSlider(value: value, minValue: range.lowerBound, maxValue: range.upperBound,
                                 target: context.coordinator, action: #selector(Coordinator.changed(_:)))
        s.isContinuous = true
        s.controlSize = .small
        s.setAccessibilityIdentifier(identifier)
        return s
    }

    func updateNSView(_ s: ResettableSlider, context: Context) {
        context.coordinator.parent = self
        s.minValue = range.lowerBound
        s.maxValue = range.upperBound
        if s.doubleValue != value { s.doubleValue = value }
        s.onBegin = { context.coordinator.parent.onBegin() }
        s.onEnd = { context.coordinator.parent.onEnd() }
        s.onReset = {
            let p = context.coordinator.parent
            if let reset = p.onReset {
                reset()
            } else {
                p.value = p.defaultValue
            }
        }
    }

    @MainActor
    final class Coordinator: NSObject {
        var parent: DevSlider
        init(_ p: DevSlider) { parent = p }
        @objc func changed(_ sender: NSSlider) { parent.value = sender.doubleValue }
    }
}

final class ResettableSlider: NSSlider {
    var onBegin: () -> Void = {}
    var onEnd: () -> Void = {}
    var onReset: () -> Void = {}

    override func mouseDown(with event: NSEvent) {
        if event.clickCount == 2 {
            onReset()
            return
        }
        // super.mouseDown はマウスを離すまで戻らない（その間の変更をまとめて 1 回の Undo にする）
        onBegin()
        super.mouseDown(with: event)
        onEnd()
    }
}

// MARK: - ヒストグラム

struct HistogramView: View {
    let histogram: [[UInt32]]?

    var body: some View {
        Canvas { ctx, size in
            ctx.fill(Path(CGRect(origin: .zero, size: size)), with: .color(.black.opacity(0.85)))
            guard let h = histogram, h.count == 3 else { return }
            // 両端（真っ黒・真っ白）は大きくなりがちなので、最大値は内側から取る
            let peak = Double(h.map { $0[1..<255].max() ?? 1 }.max() ?? 1)
            let colors: [Color] = [.red, .green, .blue]
            ctx.blendMode = .plusLighter
            for (ch, color) in colors.enumerated() {
                var path = Path()
                path.move(to: CGPoint(x: 0, y: size.height))
                for i in 0..<256 {
                    let v = min(1, (Double(h[ch][i]) / peak).squareRoot())
                    path.addLine(to: CGPoint(x: size.width * Double(i) / 255, y: size.height * (1 - v)))
                }
                path.addLine(to: CGPoint(x: size.width, y: size.height))
                path.closeSubpath()
                ctx.fill(path, with: .color(color.opacity(0.7)))
            }
        }
        .frame(height: 72)
        .clipShape(RoundedRectangle(cornerRadius: 4))
        .accessibilityIdentifier("histogram")
    }
}

extension View {
    /// AppKit のコントロールにベースラインを問い合わせない（揃え位置は中央）。
    /// SwiftUI は行の揃えのために NSSlider のベースラインを毎回問い合わせ、その処理（外観の解決）が重い
    func baselineAtCenter() -> some View {
        alignmentGuide(.firstTextBaseline) { $0[VerticalAlignment.center] }
            .alignmentGuide(.lastTextBaseline) { $0[VerticalAlignment.center] }
    }
}
