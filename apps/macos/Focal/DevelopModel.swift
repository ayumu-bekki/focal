import AppKit
import FocalCore
import Observation
import os

/// 現像ビューアの状態（5.3 章・5.7 章・9.3 章）。画像処理はすべて core が行い、ここは要求と表示の管理だけ（ADR-10）。
@MainActor
@Observable
final class DevelopModel {
    enum Zoom: Equatable {
        case fit
        /// 100%: 1 画像画素 = 1 デバイス画素（5.6 章）。center は出力画像の座標
        case actual(center: CGPoint)
    }

    // MARK: 表示の状態

    private(set) var photoID: Int64?
    /// 現像できる写真か（RAW だけ。JPEG などは表示だけ。v3.22）
    private(set) var isEditable = true
    private(set) var stage: SessionInfo.Stage = .opening
    private(set) var info: SessionInfo?
    /// UI が編集する値（core の Settings の写し）
    private(set) var settings = DevelopSettings()
    /// フィット表示の画像（デコード前は埋め込みプレビュー）
    private(set) var fitImage: CGImage?
    private(set) var fitIsPreview = true
    /// 100% 表示の描画済み範囲（出力画像の座標）
    private(set) var region: (image: CGImage, x: Double, y: Double)?
    private(set) var histogram: [[UInt32]]?
    private(set) var errorMessage: String?
    var zoom: Zoom = .fit {
        didSet { if zoom != oldValue { requestRender() } }
    }
    private(set) var canUndo = false
    private(set) var canRedo = false

    /// クロップモード（5.6 章）: 全体を描き、枠を重ねて表示する。入ってから出るまでが 1 回の Undo
    private(set) var cropMode = false
    /// 水平線ツール（クロップモード中）: ドラッグで引いた線が水平になるよう傾き補正する
    var levelTool = false
    private var cropStart: DevelopSettings?
    /// テキスト入力中は ⌘Z をテキストの Undo に渡す（9.3 章）
    var isEditingText = false

    /// 計測値（ログと FOCAL_FRAME_STATS の表示用）
    private(set) var openToFirstImageMs: Double?
    private(set) var openToPreviewMs: Double?
    private(set) var openToReadyMs: Double?
    private(set) var lastRenderMs: Double?
    /// 編集操作から、その結果が表示されるまでの時間（直近 50 回）
    private var editLatencies: [Double] = []

    // MARK: 内部

    let editor: Editor
    private var session: Session?
    private var openedAt: CFTimeInterval = 0
    private var renderToken = 0
    /// ビューの大きさ（デバイス画素）と backingScaleFactor
    private(set) var viewPixels = CGSize(width: 1600, height: 1000)
    private(set) var backingScale: CGFloat = 2
    private var proxyResizeTask: Task<Void, Never>?
    private let log = Logger(subsystem: AppPaths.bundleID, category: "develop")

    init(catalog: Catalog) throws {
        editor = try Editor(catalog: catalog)
    }

    /// 現像結果のサムネイルのキャッシュ（10 章）。作り終えたら onUpdated（メインスレッド）
    func setThumbnailCache(_ dir: URL, onUpdated: @escaping @MainActor (Int64) -> Void) throws {
        try editor.setThumbnailCache(dir) { id in Task { @MainActor in onUpdated(id) } }
    }

    /// 表示している画像の大きさ（出力画像。クロップモードではキャンバス）
    var outputSize: CGSize {
        guard let i = info, i.outputWidth > 0 else { return .zero }
        return cropMode ? canvasSize : CGSize(width: i.outputWidth, height: i.outputHeight)
    }

    var canvasSize: CGSize {
        guard let i = info else { return .zero }
        return CGSize(width: i.canvasWidth, height: i.canvasHeight)
    }

    private var proxyLongEdge: Int { min(3840, Int(max(viewPixels.width, viewPixels.height))) }

    // MARK: 開く・閉じる

    /// 写真を開く（5.3 章）。next があれば 1 枚だけ先読みする。
    /// placeholder（グリッドのサムネイル）があれば、埋め込みプレビューが出るまでそれを表示する
    func open(photoID id: Int64, next: Int64?, placeholder: CGImage? = nil, editable: Bool = true) {
        if id == photoID {
            isEditable = editable
            return
        }
        close()
        photoID = id
        isEditable = editable
        stage = .opening
        info = nil
        fitImage = placeholder
        fitIsPreview = true
        region = nil
        histogram = nil
        errorMessage = nil
        zoom = .fit
        openToPreviewMs = nil
        openToReadyMs = nil
        openToFirstImageMs = placeholder == nil ? nil : 0
        editLatencies = []
        editCoreLatencies = []
        editWaitLatencies = []
        openedAt = CACurrentMediaTime()
        do {
            let s = try editor.open(photoID: id, previewLongEdge: proxyLongEdge, proxyLongEdge: proxyLongEdge) {
                [weak self] event in
                Task { @MainActor in self?.handle(event, photoID: id) }
            }
            session = s
            settings = s.settings
            syncBaseline = settings
            refreshUndoState()
        } catch {
            stage = .failed
            errorMessage = String(describing: error)
        }
        if let next { editor.prefetch(photoID: next) }
    }

    /// 編集をすぐ保存する（書き出しの直前）
    func saveNow() { session?.saveNow() }

    /// 編集をすぐ保存する（写真の切り替え時、7.4 章）
    func close() {
        if cropMode { commitCrop() }
        session?.close()
        session = nil
        photoID = nil
        renderToken += 1
    }

    private func handle(_ event: Session.Event, photoID id: Int64) {
        guard id == photoID, let session else { return }
        let ms = (CACurrentMediaTime() - openedAt) * 1000
        switch event {
        case .preview:
            if stage == .opening, let img = session.previewImage() {
                info = session.info
                stage = .preview
                fitImage = img
                fitIsPreview = true
                openToPreviewMs = ms
                if openToFirstImageMs == nil { openToFirstImageMs = ms }
                log.info("open→preview \(ms, format: .fixed(precision: 1)) ms (photo \(id))")
            }
        case .ready:
            info = session.info
            stage = .ready
            openToReadyMs = ms
            if openToFirstImageMs == nil { openToFirstImageMs = ms }
            log.info("open→ready \(ms, format: .fixed(precision: 1)) ms (photo \(id))")
            requestRender()
        case .failed(let message):
            stage = .failed
            errorMessage = message
        }
    }

    // MARK: 編集（9.3 章）

    /// スライダーのドラッグ中（beginChange 〜 endChange）
    private var changing = false

    // MARK: 現像パラメータの同期（9.5 章）

    /// オンの間、動かした調整の項目だけを、選択中の他の写真へ同じ値で写す（Lightroom Classic の Auto Sync）。
    /// ドラッグ中は開いている写真だけを描き、ドラッグの終わり（数値の入力・リセットなどはその場）に他の写真へ書く。
    /// 他の写真の変更は Undo の対象外。LibraryModel が、写す先がなくなったときや現像画面を出るときにオフにする
    var autoSync = false {
        didSet { if autoSync, !oldValue { syncBaseline = settings } }
    }
    /// 写す先になる写真の数（選択中から開いている写真を除いたもの。LibraryModel が更新する）
    var syncTargetCount = 0
    /// 動かした項目を他の写真に渡す（settings、項目）。LibraryModel が設定する
    var onSync: ((DevelopSettings, DevelopSettings.AdjustmentMask) -> Void)?
    /// 最後に同期した（または同期の対象にしなかった）時点の設定。ここからの差が「動かした項目」
    private var syncBaseline = DevelopSettings()

    private func commitSync() {
        guard autoSync, isEditable else { return }
        let mask = settings.changedAdjustments(from: syncBaseline)
        guard mask != 0 else { return }
        syncBaseline = settings
        onSync?(settings, mask)
    }

    func beginChange() {
        changing = true
        session?.beginChange()
    }

    func endChange() {
        session?.endChange()
        refreshUndoState()
        changing = false
        commitSync()
        // 100% 表示では、ドラッグ中に省いた縮小表示（ヒストグラム）を描く
        if case .actual = zoom { requestRender() }
    }

    /// 値を変えて描き直す。beginChange 〜 endChange の外なら 1 回の Undo になる。
    /// render が false なら描き直さない（クロップモードで枠だけ動かしたとき）
    func update(render: Bool = true, _ change: (inout DevelopSettings) -> Void) {
        guard let session, isEditable else { return }
        var s = settings
        change(&s)
        guard s != settings else { return }
        settings = s
        session.setSettings(s)
        refreshInfo()
        refreshUndoState()
        if !changing { commitSync() }
        if render { requestRender(measureEdit: true) }
    }

    /// プリセットの調整を重ねる（切り取り・回転・傾きはそのまま）。1 回の Undo になる
    func applyPreset(_ id: String, using presets: PresetStore) throws {
        guard session != nil, isEditable, stage == .ready, !cropMode else { return }
        let merged = try presets.applying(id: id, to: settings)
        update { $0 = merged }
    }

    /// 調整をすべて初期値に戻す（切り取り・回転・傾きはそのまま）。1 回の Undo になる
    func resetAdjustments() {
        update { s in
            let d = DevelopSettings()
            s.customWhiteBalance = d.customWhiteBalance
            s.temperature = d.temperature
            s.tint = d.tint
            s.exposure = d.exposure
            s.contrast = d.contrast
            s.highlights = d.highlights
            s.shadows = d.shadows
            s.whites = d.whites
            s.blacks = d.blacks
            s.brightness = d.brightness
            s.saturation = d.saturation
            s.vibrance = d.vibrance
            s.clarity = d.clarity
            s.sharpness = d.sharpness
            s.noiseReduction = d.noiseReduction
            s.colorNoiseReduction = d.colorNoiseReduction
        }
    }

    // MARK: ジオメトリ（5.6 章、9.2 章）

    func toggleCropMode() { cropMode ? commitCrop() : enterCrop() }

    func enterCrop() {
        guard let session, isEditable, stage == .ready, !cropMode else { return }
        session.beginChange()
        cropStart = settings
        cropMode = true
        zoom = .fit
        region = nil
        // まだ切り取っていない写真は、縦横比の既定を「元の比率」にする（自由のままの切り取りを保つため、
        // すでに枠や傾き補正がある写真は変えない）
        if settings.aspect == Self.aspectFree && Self.isUncropped(settings) { setAspect(Self.aspectOriginal) }
        requestRender()
    }

    private static func isUncropped(_ s: DevelopSettings) -> Bool {
        s.cropX <= 0.0001 && s.cropY <= 0.0001 && s.cropW >= 0.9999 && s.cropH >= 0.9999 && s.straighten == 0
    }

    /// core の AspectMode と同じ値（CropToolbar の縦横比の一覧と同じ）
    private static let aspectFree: Int32 = 0
    private static let aspectOriginal: Int32 = 1

    /// 確定（1 回の Undo になる）
    func commitCrop() {
        guard let session, cropMode else { return }
        // 切り取らずに確定したときは、入るときに自動で選んだ「元の比率」を戻す（編集のない写真に編集を作らない）
        if let start = cropStart, start.aspect == Self.aspectFree, settings.aspect == Self.aspectOriginal,
           Self.isUncropped(settings) {
            update(render: false) { $0.aspect = Self.aspectFree }
        }
        cropMode = false
        levelTool = false
        cropStart = nil
        session.endChange()
        refreshInfo()
        refreshUndoState()
        requestRender()
    }

    /// 取り消し（クロップモードに入る前の状態に戻す。Undo には積まない）
    func cancelCrop() {
        guard let session, cropMode, let start = cropStart else { return }
        settings = start
        session.setSettings(start)
        session.endChange()
        cropMode = false
        levelTool = false
        cropStart = nil
        refreshInfo()
        refreshUndoState()
        requestRender()
    }

    /// クロップ枠のハンドルをドラッグした（start はドラッグ開始時の設定、dx / dy は正規化座標）
    func dragCrop(from start: DevelopSettings, handle: CropTool.Handle, dx: Double, dy: Double) {
        let canvas = canvasSize
        guard canvas.width > 0 else { return }
        let next = CropTool.drag(start, canvas: canvas, handle: handle, dx: dx, dy: dy)
        update(render: false) { $0 = next }
    }

    /// 傾き補正（枠は余白が出ない最大の大きさに合わせる）
    func setStraighten(_ deg: Double) {
        let canvas = canvasSize
        update { s in
            s.straighten = deg
            s = CropTool.fit(s, canvas: canvas)
        }
    }

    func setAspect(_ aspect: Int32) {
        let canvas = canvasSize
        update(render: false) { s in
            s.aspect = aspect
            s = CropTool.fit(s, canvas: canvas)
        }
    }

    /// 水平線ツール: キャンバス上の 2 点（画素）
    func level(from a: CGPoint, to b: CGPoint) {
        setStraighten(CropTool.straighten(from: a, to: b, current: settings.straighten))
    }

    /// 90° 回転（[ / ]）。クロップモードの外なら 1 回の Undo
    func rotate(by steps: Int) {
        guard stage == .ready else { return }
        update { $0 = CropTool.rotate($0, steps: steps) }
        if case .actual = zoom { zoom = .fit }
    }

    /// WB をカスタムにするときは、撮影時の色温度・色かぶりから始める
    func setCustomWhiteBalance(_ on: Bool) {
        update { s in
            if on && !s.customWhiteBalance, let i = info, i.asShotTemperature > 0 {
                s.temperature = (i.asShotTemperature / 50).rounded() * 50
                s.tint = i.asShotTint.rounded()
            }
            s.customWhiteBalance = on
        }
    }

    func resetWhiteBalance() { update { $0.customWhiteBalance = false } }

    /// 色温度・色かぶりの変更。撮影時の設定のときは、撮影時の値から始めてカスタムに切り替える
    func updateWhiteBalance(_ change: (inout DevelopSettings) -> Void) {
        update { s in
            if !s.customWhiteBalance {
                if let i = info, i.asShotTemperature > 0 {
                    s.temperature = (i.asShotTemperature / 10).rounded() * 10
                    s.tint = i.asShotTint.rounded()
                }
                s.customWhiteBalance = true
            }
            change(&s)
        }
    }

    func resetLight() {
        update { s in
            let d = DevelopSettings()
            s.exposure = d.exposure
            s.contrast = d.contrast
            s.highlights = d.highlights
            s.shadows = d.shadows
            s.whites = d.whites
            s.blacks = d.blacks
            s.brightness = d.brightness
        }
    }

    func resetColor() {
        update { s in
            let d = DevelopSettings()
            s.saturation = d.saturation
            s.vibrance = d.vibrance
        }
    }

    func resetDetail() {
        update { s in
            let d = DevelopSettings()
            s.clarity = d.clarity
            s.sharpness = d.sharpness
            s.noiseReduction = d.noiseReduction
            s.colorNoiseReduction = d.colorNoiseReduction
        }
    }

    func resetLens() {
        update { s in
            let d = DevelopSettings()
            s.lensEnabled = d.lensEnabled
            s.lensID = d.lensID
            s.lensDistortion = d.lensDistortion
            s.lensTCA = d.lensTCA
            s.lensVignetting = d.lensVignetting
            s.lensProjection = d.lensProjection
        }
    }

    /// 写真の EXIF のレンズ情報と、自動で選ばれるレンズ（写真を開いたとき・レンズ補正の区分を開いたときに読む）
    var lensExif: String { session?.lensExif ?? "" }
    var detectedLens: LensInfo? { session?.detectedLens }

    func undo() {
        guard let session, session.undo() else { return }
        settings = session.settings
        syncBaseline = settings  // 取り消し・やり直しは他の写真へ写さない
        refreshUndoState()
        requestRender()
    }

    func redo() {
        guard let session, session.redo() else { return }
        settings = session.settings
        syncBaseline = settings  // 取り消し・やり直しは他の写真へ写さない
        refreshUndoState()
        requestRender()
    }

    // Observation は同じ値を代入しても変更を通知する（読んでいるビューが描き直される）ので、変わったときだけ代入する
    private func refreshUndoState() {
        let u = session?.canUndo ?? false, r = session?.canRedo ?? false
        if canUndo != u { canUndo = u }
        if canRedo != r { canRedo = r }
    }

    private func refreshInfo() {
        guard let session else { return }
        let i = session.info
        if info != i { info = i }
    }

    // MARK: ビューの大きさ・ズーム

    /// ビューの大きさが変わった。プロキシは 300ms 待ってから作り直す（5.2 章）
    func setViewSize(pixels: CGSize, backingScale scale: CGFloat) {
        guard pixels.width > 0, pixels.height > 0, pixels != viewPixels || scale != backingScale else { return }
        viewPixels = pixels
        backingScale = scale
        proxyResizeTask?.cancel()
        proxyResizeTask = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(300))
            guard let self, !Task.isCancelled, let session = self.session else { return }
            session.setProxyLongEdge(self.proxyLongEdge)
            self.requestRender()
        }
        if case .actual = zoom { requestRender() }
    }

    /// Z: フィット ⇄ 100%。at は出力画像の座標（ポインタの位置。なければ中央）
    func toggleZoom(at point: CGPoint?) {
        guard stage == .ready || stage == .preview, !cropMode else { return }
        switch zoom {
        case .fit:
            let size = outputSize
            guard size.width > 0 else { return }
            zoom = .actual(center: point ?? CGPoint(x: size.width / 2, y: size.height / 2))
        case .actual:
            zoom = .fit
            region = nil
        }
    }

    /// 100% 表示でドラッグした。delta はデバイス画素
    func pan(by delta: CGSize) {
        guard case .actual(let c) = zoom else { return }
        let size = outputSize
        let half = CGSize(width: viewPixels.width / 2, height: viewPixels.height / 2)
        // 画像の端より外に中心を動かさない（小さい画像は中央に固定）
        func clamp(_ v: CGFloat, _ total: CGFloat, _ h: CGFloat) -> CGFloat {
            total <= 2 * h ? total / 2 : min(max(v, h), total - h)
        }
        zoom = .actual(center: CGPoint(x: clamp(c.x - delta.width, size.width, half.width),
                                       y: clamp(c.y - delta.height, size.height, half.height)))
    }

    // MARK: レンダリング（latest-wins は core が行う）

    func requestRender(measureEdit: Bool = false) {
        guard let session, stage == .ready else { return }
        renderToken += 1
        let token = renderToken
        let fitW = max(1, Int(viewPixels.width)), fitH = max(1, Int(viewPixels.height))
        var regionTarget: Session.RenderTarget?
        if !cropMode, case .actual(let c) = zoom {
            // 表示範囲に上下左右 25% の余白を足した範囲（5.7 章）
            let w = viewPixels.width * 1.5, h = viewPixels.height * 1.5
            regionTarget = .region(x: max(0, c.x - w / 2), y: max(0, c.y - h / 2), width: Int(w), height: Int(h))
        }
        let fitTarget = Session.RenderTarget.fit(maxWidth: fitW, maxHeight: fitH)
        // 100% 表示でスライダーをドラッグしている間は縮小表示を描かない（見えていないので、100% の範囲の更新を優先する）。
        // ヒストグラムは手を離したときに描き直す
        let skipFit = changing && regionTarget != nil
        let startUptime = ProcessInfo.processInfo.systemUptime
        let ignoreCrop = cropMode
        let gate = renderGate

        // 最初の描画はその場で頼む（メインスレッドで SwiftUI の更新が終わるのを待たない）。
        // core は latest-wins なので、100% の範囲 → フィットの順に頼む続き（フィット）は、新しい要求が来ていなければ出す
        let first = gate.start(token) {
            regionTarget.map { session.startRender($0) } ?? session.startRender(fitTarget, ignoreCrop: ignoreCrop)
        }
        Task.detached(priority: .userInitiated) { [weak self] in
            if regionTarget != nil {
                let out = await first.value()
                await MainActor.run { self?.applyRegion(out, token: token, startUptime: startUptime,
                                                        measure: skipFit && measureEdit) }
                if skipFit { return }
                guard let fit = gate.startIfCurrent(token, { session.startRender(fitTarget, ignoreCrop: ignoreCrop) })
                else { return }
                let fout = await fit.value()
                await MainActor.run { self?.applyFit(fout, token: token, startUptime: startUptime, isMain: false,
                                                     measure: measureEdit) }
            } else {
                let out = await first.value()
                await MainActor.run { self?.applyFit(out, token: token, startUptime: startUptime, isMain: true,
                                                     measure: measureEdit) }
            }
        }
    }

    /// 描画の依頼の順番を守る（core は latest-wins: 古い要求の続きが新しい要求を打ち切らないようにする）
    private let renderGate = RenderGate()

    private func applyRegion(_ out: Session.RenderOutcome, token: Int, startUptime: TimeInterval, measure: Bool) {
        guard token == renderToken, case .image(let r) = out else { return }
        region = (r.image, r.regionX, r.regionY)
        let ms = (ProcessInfo.processInfo.systemUptime - startUptime) * 1000
        lastRenderMs = ms
        if measure { recordEdit(ms, render: r, startUptime: startUptime) }
    }

    /// isMain: フィットが主な表示（100% 表示ではない）
    private func applyFit(_ out: Session.RenderOutcome, token: Int, startUptime: TimeInterval, isMain: Bool,
                          measure: Bool) {
        guard token == renderToken, case .image(let r) = out else { return }
        fitImage = r.image
        fitIsPreview = false
        histogram = r.histogram
        let ms = (ProcessInfo.processInfo.systemUptime - startUptime) * 1000
        if isMain { lastRenderMs = ms }
        if measure { recordEdit(ms, render: r, startUptime: startUptime) }
    }

    /// スライダー操作から表示までの時間（ms）の内訳: core に頼むまでの待ち、core が描く時間
    private var editWaitLatencies: [Double] = []
    private var editCoreLatencies: [Double] = []
    private func recordEdit(_ ms: Double, render r: Session.RenderedImage, startUptime: TimeInterval) {
        recordEdit(ms, wait: (r.submittedAt - startUptime) * 1000, core: (r.finishedAt - r.submittedAt) * 1000)
    }

    private func recordEdit(_ ms: Double, wait: Double, core: Double) {
        editLatencies.append(ms)
        editWaitLatencies.append(wait)
        editCoreLatencies.append(core)
        for i in [0, 1, 2] where [editLatencies.count, editWaitLatencies.count, editCoreLatencies.count][i] > 50 {
            if i == 0 { editLatencies.removeFirst() } else if i == 1 { editWaitLatencies.removeFirst() } else { editCoreLatencies.removeFirst() }
        }
    }

    /// 計測値の表示（UI テストが読む）
    var timingSummary: String {
        func f(_ v: Double?) -> String { v.map { String(format: "%.0f", $0) } ?? "-" }
        let sorted = editLatencies.sorted(), core = editCoreLatencies.sorted()
        let p50 = sorted.isEmpty ? nil : sorted[sorted.count / 2]
        let p90 = sorted.isEmpty ? nil : sorted[min(sorted.count - 1, sorted.count * 9 / 10)]
        let c50 = core.isEmpty ? nil : core[core.count / 2]
        let waits = editWaitLatencies.sorted()
        let w50 = waits.isEmpty ? nil : waits[waits.count / 2]
        return "stage=\(stage) first_ms=\(f(openToFirstImageMs)) preview_ms=\(f(openToPreviewMs)) "
            + "ready_ms=\(f(openToReadyMs)) render_ms=\(f(lastRenderMs)) edit_n=\(sorted.count) "
            + "edit_p50_ms=\(f(p50)) edit_p90_ms=\(f(p90)) wait_p50_ms=\(f(w50)) core_p50_ms=\(f(c50)) "
            + "gpu=\(editor.gpuName.isEmpty ? "-" : editor.gpuName)"
    }
}

/// 描画の依頼の順番（DevelopModel.requestRender）。依頼を出す操作と「最新の要求か」の確認を同じロックの中で行う
final class RenderGate: @unchecked Sendable {
    private let lock = NSLock()
    private var current = 0

    /// 新しい要求として依頼を出す
    func start(_ token: Int, _ submit: () -> PendingRender) -> PendingRender {
        lock.lock()
        defer { lock.unlock() }
        current = token
        return submit()
    }

    /// まだ最新の要求なら続きの依頼を出す。新しい要求が来ていれば出さない（nil）
    func startIfCurrent(_ token: Int, _ submit: () -> PendingRender) -> PendingRender? {
        lock.lock()
        defer { lock.unlock() }
        return token == current ? submit() : nil
    }
}
