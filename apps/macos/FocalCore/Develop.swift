import CFocal
import CoreGraphics
import Foundation

/// 編集パラメータ（6.1 章）。未知のキーは core 側のセッションが保持する
public struct DevelopSettings: Equatable, Sendable {
    public var processVersion: Int32
    public var customWhiteBalance: Bool
    public var temperature: Double
    public var tint: Double
    public var exposure: Double
    public var contrast: Double
    public var highlights: Double
    public var shadows: Double
    public var whites: Double
    public var blacks: Double
    public var brightness: Double
    public var saturation: Double
    public var vibrance: Double
    public var clarity: Double
    public var sharpness: Double
    public var noiseReduction: Double
    public var colorNoiseReduction: Double
    public var rotate90: Int32
    public var straighten: Double
    public var cropX, cropY, cropW, cropH: Double
    public var aspect: Int32

    public init() {
        var c = fc_settings()
        fc_settings_init(&c)
        self.init(c)
    }

    /// 調整がすべて初期値か（切り取り・回転・傾きは見ない）
    public var hasNoAdjustments: Bool {
        let d = DevelopSettings()
        return customWhiteBalance == d.customWhiteBalance && exposure == d.exposure && contrast == d.contrast
            && highlights == d.highlights && shadows == d.shadows && whites == d.whites && blacks == d.blacks
            && brightness == d.brightness && saturation == d.saturation && vibrance == d.vibrance
            && clarity == d.clarity && sharpness == d.sharpness && noiseReduction == d.noiseReduction
            && colorNoiseReduction == d.colorNoiseReduction
    }

    init(_ c: fc_settings) {
        processVersion = c.process_version
        customWhiteBalance = c.wb_mode == Int32(FC_WB_CUSTOM.rawValue)
        temperature = c.temperature
        tint = c.tint
        exposure = c.exposure
        contrast = c.contrast
        highlights = c.highlights
        shadows = c.shadows
        whites = c.whites
        blacks = c.blacks
        brightness = c.brightness
        saturation = c.saturation
        vibrance = c.vibrance
        clarity = c.clarity
        sharpness = c.sharpness
        noiseReduction = c.noise_reduction
        colorNoiseReduction = c.color_noise_reduction
        rotate90 = c.rotate90
        straighten = c.straighten
        cropX = c.crop_x
        cropY = c.crop_y
        cropW = c.crop_w
        cropH = c.crop_h
        aspect = c.aspect
    }

    var c: fc_settings {
        var s = fc_settings()
        s.process_version = processVersion
        s.wb_mode = customWhiteBalance ? Int32(FC_WB_CUSTOM.rawValue) : Int32(FC_WB_AS_SHOT.rawValue)
        s.temperature = temperature
        s.tint = tint
        s.exposure = exposure
        s.contrast = contrast
        s.highlights = highlights
        s.shadows = shadows
        s.whites = whites
        s.blacks = blacks
        s.brightness = brightness
        s.saturation = saturation
        s.vibrance = vibrance
        s.clarity = clarity
        s.sharpness = sharpness
        s.noise_reduction = noiseReduction
        s.color_noise_reduction = colorNoiseReduction
        s.rotate90 = rotate90
        s.straighten = straighten
        s.crop_x = cropX
        s.crop_y = cropY
        s.crop_w = cropW
        s.crop_h = cropH
        s.aspect = aspect
        return s
    }
}

public struct SessionInfo: Sendable, Equatable {
    public enum Stage: Int32, Sendable { case opening = 0, preview, ready, failed }
    public let stage: Stage
    public let orientedWidth, orientedHeight: Int
    /// ジオメトリ適用後のフル解像度
    public let outputWidth, outputHeight: Int
    /// 回転・傾き補正後、クロップ前（クロップ枠の座標の基準）
    public let canvasWidth, canvasHeight: Int
    public let asShotTemperature, asShotTint: Double
    public let previewWidth, previewHeight: Int
    /// プレビューが前に表示したときの現像結果（Display P3、ジオメトリ適用済み）なら true。
    /// false ならカメラの埋め込みプレビュー（sRGB）
    public let previewIsDisplayP3: Bool
}

/// 現像ビューア（fc_editor）。開いているセッションより長く生かすこと（Session が参照を持つ）
public final class Editor: @unchecked Sendable {
    let handle: OpaquePointer
    private let catalog: Catalog

    public init(catalog: Catalog) throws {
        var h: OpaquePointer?
        try check(fc_editor_create(catalog.handle, &h))
        handle = h!
        self.catalog = catalog
    }

    deinit {
        fc_editor_destroy(handle)  // 閉じた写真のキャッシュを書き終えるまで待つ（その間 onUpdated が呼ばれうる）
        thumbnailBox?.release()
    }

    /// 写真を開く（5.3 章）。events はワーカースレッドから呼ばれる
    public func open(photoID: Int64, previewLongEdge: Int, proxyLongEdge: Int,
                     events: @escaping @Sendable (Session.Event) -> Void) throws -> Session {
        let box = EventBox(events)
        let user = Unmanaged.passRetained(box).toOpaque()
        var h: OpaquePointer?
        let st = fc_editor_open(handle, photoID, Int32(previewLongEdge), Int32(proxyLongEdge), sessionEvent, user, &h)
        if st != FC_OK {
            Unmanaged<EventBox>.fromOpaque(user).release()
            throw FocalError(status: st)
        }
        return Session(handle: h!, editor: self, photoID: photoID, events: Unmanaged<EventBox>.fromOpaque(user))
    }

    /// 次の写真を 1 枚だけ先読みする
    public func prefetch(photoID: Int64) { fc_editor_prefetch(handle, photoID) }

    private var thumbnailBox: Unmanaged<ThumbnailUpdateBox>?

    /// 編集後のサムネイルを作るキャッシュ（10 章）。onUpdated はワーカースレッドから呼ばれる
    public func setThumbnailCache(_ dir: URL, onUpdated: @escaping @Sendable (Int64) -> Void) throws {
        let box = Unmanaged.passRetained(ThumbnailUpdateBox(onUpdated))
        try dir.path.withCString {
            try check(fc_editor_set_thumbnail_cache(handle, $0, { user, id in
                Unmanaged<ThumbnailUpdateBox>.fromOpaque(user!).takeUnretainedValue().fn(id)
            }, box.toOpaque()))
        }
        thumbnailBox?.release()
        thumbnailBox = box
    }

    /// 表示（フィット・100%）を描く GPU の名前（v3.14）。CPU で描くなら空
    public var gpuName: String { String(cString: fc_editor_gpu_name(handle)) }

    /// 表示用の大きいプレビューのキャッシュ。写真を閉じるとき現像結果を入れ、次に開いたときすぐ出す
    public func setPreviewCache(_ dir: URL, limitBytes: UInt64) throws {
        try dir.path.withCString { try check(fc_editor_set_preview_cache(handle, $0, limitBytes)) }
    }

    public func setPreviewCacheLimit(_ bytes: UInt64) throws {
        try check(fc_editor_set_preview_cache_limit(handle, bytes))
    }

    /// 使用量（バイト）。フォルダを数えるので、メインスレッドから呼ばないこと
    public func previewCacheUsage() throws -> UInt64 {
        var n: UInt64 = 0
        try check(fc_editor_preview_cache_usage(handle, &n))
        return n
    }

    public func clearPreviewCache() throws {
        try check(fc_editor_clear_preview_cache(handle))
    }
}

final class ThumbnailUpdateBox: @unchecked Sendable {
    let fn: @Sendable (Int64) -> Void
    init(_ fn: @escaping @Sendable (Int64) -> Void) { self.fn = fn }
}

/// クロップモードの操作（5.6 章）。計算は core が行う。枠はキャンバス上の正規化座標
public enum CropTool {
    public enum Handle: Int32, Sendable {
        case move = 0, left, right, top, bottom, topLeft, topRight, bottomLeft, bottomRight
    }

    /// ハンドルのドラッグ（start はドラッグ開始時の設定）
    public static func drag(_ start: DevelopSettings, canvas: CGSize, handle: Handle, dx: Double, dy: Double)
        -> DevelopSettings {
        var c = start.c
        var out = fc_settings()
        guard fc_crop_drag(&c, Int32(canvas.width), Int32(canvas.height), handle.rawValue, dx, dy, &out) == FC_OK
        else { return start }
        return DevelopSettings(out)
    }

    /// 縦横比と傾き補正に合わせた最大の枠にする
    public static func fit(_ s: DevelopSettings, canvas: CGSize) -> DevelopSettings {
        var c = s.c
        guard fc_crop_fit(&c, Int32(canvas.width), Int32(canvas.height)) == FC_OK else { return s }
        return DevelopSettings(c)
    }

    /// 時計回りに 90° × steps（クロップ枠も一緒に回す）
    public static func rotate(_ s: DevelopSettings, steps: Int) -> DevelopSettings {
        var c = s.c
        guard fc_crop_rotate(&c, Int32(steps)) == FC_OK else { return s }
        return DevelopSettings(c)
    }

    /// 水平線ツール: キャンバス上の 2 点（画素）から傾き補正値
    public static func straighten(from a: CGPoint, to b: CGPoint, current: Double) -> Double {
        fc_crop_straighten_from_line(a.x, a.y, b.x, b.y, current)
    }
}

final class EventBox: @unchecked Sendable {
    let fn: @Sendable (Session.Event) -> Void
    init(_ fn: @escaping @Sendable (Session.Event) -> Void) { self.fn = fn }
}

private func sessionEvent(_ user: UnsafeMutableRawPointer?, _ event: Int32, _ message: UnsafePointer<CChar>?) {
    let box = Unmanaged<EventBox>.fromOpaque(user!).takeUnretainedValue()
    switch event {
    case Int32(FC_EVENT_PREVIEW.rawValue): box.fn(.preview)
    case Int32(FC_EVENT_READY.rawValue): box.fn(.ready)
    default: box.fn(.failed(String(optionalCString: message) ?? ""))
    }
}

/// 写真 1 枚の編集セッション（fc_session）。close() するか解放されると編集を保存する
public final class Session: @unchecked Sendable {
    public enum Event: Sendable { case preview, ready, failed(String) }

    public enum RenderOutcome: Sendable {
        case image(RenderedImage)
        case cancelled  // 新しい要求に追い越された（latest-wins）
        case notReady
        case failed
    }

    public struct RenderedImage: @unchecked Sendable {
        public let image: CGImage  // Display P3 をタグ付け
        public let scale: Double   // 出力画像（フル解像度）に対する縮尺
        public let regionX, regionY: Double
        /// R, G, B 各 256 段階
        public let histogram: [[UInt32]]
        /// core に描画を頼んだ時刻と、描き終えた時刻（ProcessInfo.systemUptime。計測用）
        public let submittedAt: TimeInterval
        public let finishedAt: TimeInterval
    }

    private var handle: OpaquePointer?
    private let lock = NSLock()
    private let editor: Editor
    private let events: Unmanaged<EventBox>
    public let photoID: Int64

    init(handle: OpaquePointer, editor: Editor, photoID: Int64, events: Unmanaged<EventBox>) {
        self.handle = handle
        self.editor = editor
        self.photoID = photoID
        self.events = events
    }

    deinit { close() }

    public func close() {
        lock.lock()
        let h = handle
        handle = nil
        lock.unlock()
        guard let h else { return }
        fc_session_close(h)  // 以降コールバックは来ない
        events.release()
    }

    private func withHandle<R>(_ body: (OpaquePointer) throws -> R) rethrows -> R? {
        lock.lock()
        defer { lock.unlock() }
        guard let h = handle else { return nil }
        return try body(h)
    }

    public var info: SessionInfo? {
        withHandle { h in
            var i = fc_session_info()
            guard fc_session_get_info(h, &i) == FC_OK else { return nil }
            return SessionInfo(stage: SessionInfo.Stage(rawValue: i.stage) ?? .failed,
                               orientedWidth: Int(i.oriented_width), orientedHeight: Int(i.oriented_height),
                               outputWidth: Int(i.output_width), outputHeight: Int(i.output_height),
                               canvasWidth: Int(i.canvas_width), canvasHeight: Int(i.canvas_height),
                               asShotTemperature: i.as_shot_temperature, asShotTint: i.as_shot_tint,
                               previewWidth: Int(i.preview_width), previewHeight: Int(i.preview_height),
                               previewIsDisplayP3: i.preview_display_p3 != 0)
        } ?? nil
    }

    /// 開いた直後のプレビュー（現像結果のキャッシュなら Display P3、埋め込みプレビューなら sRGB をタグ付け）
    public func previewImage() -> CGImage? {
        guard let i = info, i.previewWidth > 0 else { return nil }
        let stride = i.previewWidth * 3
        let data = NSMutableData(length: stride * i.previewHeight)!
        let ok = withHandle { h in
            fc_session_copy_preview(h, data.mutableBytes.assumingMemoryBound(to: UInt8.self), stride, data.length) == FC_OK
        } ?? false
        guard ok, let provider = CGDataProvider(data: data) else { return nil }
        return CGImage(width: i.previewWidth, height: i.previewHeight, bitsPerComponent: 8, bitsPerPixel: 24,
                       bytesPerRow: stride,
                       space: CGColorSpace(name: i.previewIsDisplayP3 ? CGColorSpace.displayP3 : CGColorSpace.sRGB)!,
                       bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue), provider: provider,
                       decode: nil, shouldInterpolate: true, intent: .defaultIntent)
    }

    // MARK: 編集

    public var settings: DevelopSettings {
        withHandle { h in
            var c = fc_settings()
            fc_session_get_settings(h, &c)
            return DevelopSettings(c)
        } ?? DevelopSettings()
    }

    /// beginChange と endChange の間なら途中経過、そうでなければそれ自体が 1 回の Undo になる
    public func setSettings(_ s: DevelopSettings) {
        var c = s.c
        _ = withHandle { fc_session_set_settings($0, &c) }
    }

    /// 編集をすぐ保存して、書き込みが終わるまで待つ（書き出しの直前など）
    public func saveNow() { _ = withHandle { fc_session_save($0) } }

    public func beginChange() { _ = withHandle { fc_session_begin_change($0) } }
    public func endChange() { _ = withHandle { fc_session_end_change($0) } }
    @discardableResult public func undo() -> Bool { withHandle { fc_session_undo($0) != 0 } ?? false }
    @discardableResult public func redo() -> Bool { withHandle { fc_session_redo($0) != 0 } ?? false }
    public var canUndo: Bool { withHandle { fc_session_can_undo($0) != 0 } ?? false }
    public var canRedo: Bool { withHandle { fc_session_can_redo($0) != 0 } ?? false }
    public func setProxyLongEdge(_ px: Int) { _ = withHandle { fc_session_set_proxy_long_edge($0, Int32(px)) } }

    // MARK: レンダリング

    public enum RenderTarget: Sendable {
        /// 出力画像全体を収める（プロキシから）
        case fit(maxWidth: Int, maxHeight: Int)
        /// 100% 表示: 出力画像の座標で x, y から width × height（フル解像度から）
        case region(x: Double, y: Double, width: Int, height: Int)
    }

    /// ignoreCrop: クロップモード（クロップを適用せずキャンバス全体を描く）
    public func render(_ target: RenderTarget, ignoreCrop: Bool) async -> RenderOutcome {
        await startRender(target, ignoreCrop: ignoreCrop).value()
    }

    /// latest-wins: 新しい要求を出すと、実行中の古い要求は .cancelled で返る
    public func render(_ target: RenderTarget) async -> RenderOutcome {
        await startRender(target).value()
    }

    /// その場で core に描画を頼み（待たない）、結果は value() で受け取る。
    /// メインスレッドの処理（SwiftUI の更新など）を待たずに描き始めたいときに使う
    public func startRender(_ target: RenderTarget, ignoreCrop: Bool = false) -> PendingRender {
        let pending = PendingRender()
        start(target, ignoreCropFlag: ignoreCrop ? 1 : 0, pending: pending)
        return pending
    }

    private func start(_ target: RenderTarget, ignoreCropFlag: Int32, pending: PendingRender) {
        let (w, h): (Int, Int)
        var rq = fc_render_request()
        rq.pixel_format = Int32(FC_PIXEL_BGRX8.rawValue)
        rq.ignore_crop = ignoreCropFlag
        switch target {
        case .fit(let mw, let mh):
            rq.mode = Int32(FC_RENDER_FIT.rawValue)
            rq.max_width = Int32(mw)
            rq.max_height = Int32(mh)
            (w, h) = (mw, mh)
        case .region(let x, let y, let rw, let rh):
            rq.mode = Int32(FC_RENDER_REGION.rawValue)
            rq.region_x = x
            rq.region_y = y
            rq.region_width = Int32(rw)
            rq.region_height = Int32(rh)
            (w, h) = (rw, rh)
        }
        guard w > 0, h > 0 else { return pending.complete(.failed) }
        let stride = w * 4
        let capacity = stride * h
        // core が書き込んだメモリをそのまま CGImage に渡す（コピーしない）。解放は CGImage が行う
        let buffer = UnsafeMutableRawPointer.allocate(byteCount: capacity, alignment: 64)
        rq.buffer = buffer.assumingMemoryBound(to: UInt8.self)
        rq.stride = stride
        rq.capacity = capacity

        let box = RenderBox(pending: pending, buffer: buffer, stride: stride)
        box.submittedAt = ProcessInfo.processInfo.systemUptime
        let user = Unmanaged.passRetained(box).toOpaque()
        let gen = withHandle { fc_session_render($0, &rq, renderDone, user) } ?? 0
        if gen == 0 {
            Unmanaged<RenderBox>.fromOpaque(user).release()
            buffer.deallocate()
            pending.complete(.failed)
        }
    }
}

/// 頼んだ描画の結果（startRender）。結果は core のスレッドから 1 回だけ届く
public final class PendingRender: @unchecked Sendable {
    private let lock = NSLock()
    private var result: Session.RenderOutcome?
    private var waiter: CheckedContinuation<Session.RenderOutcome, Never>?

    func complete(_ r: Session.RenderOutcome) {
        lock.lock()
        if let w = waiter {
            waiter = nil
            lock.unlock()
            w.resume(returning: r)
        } else {
            result = r
            lock.unlock()
        }
    }

    public func value() async -> Session.RenderOutcome {
        await withCheckedContinuation { c in
            lock.lock()
            if let r = result {
                lock.unlock()
                c.resume(returning: r)
            } else {
                waiter = c
                lock.unlock()
            }
        }
    }
}

private final class RenderBox: @unchecked Sendable {
    let pending: PendingRender
    let buffer: UnsafeMutableRawPointer
    let stride: Int
    var submittedAt: TimeInterval = 0  // 計測用
    init(pending: PendingRender, buffer: UnsafeMutableRawPointer, stride: Int) {
        self.pending = pending
        self.buffer = buffer
        self.stride = stride
    }
}

private let displayP3 = CGColorSpace(name: CGColorSpace.displayP3)!

private func renderDone(_ user: UnsafeMutableRawPointer?, _ status: fc_status, _ result: UnsafePointer<fc_render_result>?) {
    let box = Unmanaged<RenderBox>.fromOpaque(user!).takeRetainedValue()
    guard status == FC_OK, let r = result?.pointee else {
        box.buffer.deallocate()
        box.pending.complete(status == FC_ERR_CANCELLED ? .cancelled : status == FC_ERR_NOT_READY ? .notReady : .failed)
        return
    }
    let w = Int(r.width), h = Int(r.height)
    let provider = CGDataProvider(dataInfo: nil, data: box.buffer, size: box.stride * h) { _, data, _ in
        data.deallocate()
    }!
    // B, G, R, X（リトルエンディアンの 32-bit、先頭はアルファなし）= Core Animation がそのまま使う形式
    let info = CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
    guard let image = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: box.stride,
                              space: displayP3, bitmapInfo: info, provider: provider, decode: nil,
                              shouldInterpolate: true, intent: .defaultIntent) else {
        box.pending.complete(.failed)
        return
    }
    var hist: [[UInt32]] = []
    withUnsafeBytes(of: r.histogram) { raw in
        let all = raw.bindMemory(to: UInt32.self)
        hist = (0..<3).map { Array(all[($0 * 256)..<($0 * 256 + 256)]) }
    }
    box.pending.complete(.image(.init(image: image, scale: r.scale, regionX: r.region_x, regionY: r.region_y,
                                            histogram: hist, submittedAt: box.submittedAt,
                                            finishedAt: ProcessInfo.processInfo.systemUptime)))
}
