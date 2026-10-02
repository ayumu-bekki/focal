import AppKit
import Observation
import QuartzCore

/// スクロール性能の計測用（9.1 章: グリッド方式の比較、M2 の完了条件）。
/// FOCAL_FRAME_STATS=1 のときだけ動く。ディスプレイの更新ごとにメインスレッドで呼ばれ、
/// 前回からの間隔が 1.5 フレームを超えたらヒッチとして数える（メインスレッドが詰まると起きる）。
@MainActor
@Observable
final class FrameStats {
    static let enabled = ProcessInfo.processInfo.environment["FOCAL_FRAME_STATS"] == "1"

    /// UI テストが読む形式: "frames=N hitches=N hitch_ms=X worst_ms=X"。
    /// 毎フレーム更新すると SwiftUI の再描画そのものがヒッチを生むので、0.5 秒おきにだけ反映する
    private(set) var summary = ""

    @ObservationIgnored private var frames = 0
    @ObservationIgnored private var hitches = 0
    @ObservationIgnored private var hitchMilliseconds = 0.0
    @ObservationIgnored private var worstMilliseconds = 0.0
    @ObservationIgnored private var link: CADisplayLink?
    @ObservationIgnored private var last: CFTimeInterval = 0
    @ObservationIgnored private var lastPublish: CFTimeInterval = 0
    @ObservationIgnored weak var view: NSView?

    private func publish() {
        // ウィンドウのあるディスプレイ（8.2 章: ディスプレイごとの色の確認で、どちらで撮ったかを残す）
        let screen = view?.window?.screen?.localizedName.replacingOccurrences(of: " ", with: "_") ?? "-"
        summary = String(format: "frames=%d hitches=%d hitch_ms=%.1f worst_ms=%.1f screen=%@", frames, hitches,
                         hitchMilliseconds, worstMilliseconds, screen)
    }

    func start(on view: NSView) {
        guard Self.enabled, link == nil else { return }
        self.view = view
        let l = view.displayLink(target: self, selector: #selector(tick(_:)))
        l.add(to: .main, forMode: .common)
        link = l
    }

    // MARK: 自動スクロールの計測（FOCAL_SCROLL_BENCH=1）

    static let benchEnabled = ProcessInfo.processInfo.environment["FOCAL_SCROLL_BENCH"] == "1"

    /// (速度 pt/s, 秒数)。正は下へ。ふつうのスクロール → 速いフリック → 上へ戻る
    @ObservationIgnored private let benchPhases: [(Double, Double)] = [(3000, 5), (15000, 3), (-8000, 4)]
    @ObservationIgnored private var benchPhase = -1
    @ObservationIgnored private var benchPhaseStart: CFTimeInterval = 0
    @ObservationIgnored private weak var benchScroll: NSScrollView?
    @ObservationIgnored private var benchDone = false
    @ObservationIgnored private var maxBenchY: CGFloat = 0

    /// ウィンドウの中で一番縦に長いスクロールビュー（= グリッド）を探して自動スクロールを始める
    func startBench(in window: NSWindow) {
        guard Self.benchEnabled, benchPhase < 0, let content = window.contentView else { return }
        func scrollViews(_ v: NSView) -> [NSScrollView] {
            (v as? NSScrollView).map { [$0] } ?? [] + v.subviews.flatMap(scrollViews)
        }
        guard let target = scrollViews(content).max(by: {
            ($0.documentView?.frame.height ?? 0) < ($1.documentView?.frame.height ?? 0)
        }) else { return }
        benchScroll = target
        reset()
        benchPhase = 0
        benchPhaseStart = 0
    }

    private func stepBench(now: CFTimeInterval, dt: CFTimeInterval) {
        guard benchPhase >= 0, !benchDone, let sv = benchScroll else { return }
        if benchPhaseStart == 0 { benchPhaseStart = now }
        maxBenchY = max(maxBenchY, sv.contentView.bounds.origin.y)
        if now - benchPhaseStart > benchPhases[benchPhase].1 {
            benchPhase += 1
            benchPhaseStart = now
            if benchPhase >= benchPhases.count {
                benchDone = true
                summary = String(format: "done max_y=%.0f ", maxBenchY) + summary
                return
            }
        }
        let clip = sv.contentView
        var origin = clip.bounds.origin
        let maxY = max(0, (sv.documentView?.frame.height ?? 0) - clip.bounds.height)
        origin.y = min(maxY, max(0, origin.y + benchPhases[benchPhase].0 * dt))
        clip.scroll(to: origin)
        sv.reflectScrolledClipView(clip)
    }

    func reset() {
        frames = 0
        hitches = 0
        hitchMilliseconds = 0
        worstMilliseconds = 0
        last = 0
        publish()
    }

    @objc private func tick(_ link: CADisplayLink) {
        let now = link.timestamp
        let expected = link.targetTimestamp - link.timestamp
        defer {
            last = now
            if !benchDone, now - lastPublish > 0.5 {
                lastPublish = now
                publish()
            }
        }
        guard last > 0, expected > 0 else { return }
        let dt = now - last
        if benchDone { return }  // 自動スクロールの計測が終わったら値を固定する
        stepBench(now: now, dt: dt)
        frames += 1
        worstMilliseconds = max(worstMilliseconds, dt * 1000)
        if dt > expected * 1.5 {
            hitches += 1
            hitchMilliseconds += (dt - expected) * 1000
        }
    }
}

/// FrameStats をウィンドウにつなぐための見えないビュー
import SwiftUI

struct FrameStatsProbe: NSViewRepresentable {
    let stats: FrameStats

    func makeNSView(context: Context) -> NSView {
        let v = ProbeView()
        v.stats = stats
        return v
    }

    func updateNSView(_ nsView: NSView, context: Context) {}

    final class ProbeView: NSView {
        var stats: FrameStats?
        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            guard let window else { return }
            stats?.start(on: self)
            // 起動直後の読み込みが落ち着いてから自動スクロールを始める
            DispatchQueue.main.asyncAfter(deadline: .now() + 3) { [weak self] in
                guard let self, let window = self.window else { return }
                self.stats?.startBench(in: window)
            }
            _ = window
        }
    }
}
