import AppKit
import FocalCore
import SwiftUI

/// SD カードなどからの取り込み（v3.19、design.md 5.10 章）の状態。コピー・検証・重複の判定・登録はすべて core が行う
@MainActor
@Observable
final class CardImportModel {
    enum Phase: Equatable { case settings, running, finished }

    private(set) var sources: [ImportSource] = []
    /// 選んだカード（ImportSource.id = マウントポイント）。nil なら chosenFolder を使う
    var selectedSourceID: String? {
        didSet { if selectedSourceID != oldValue { summarize(); loadShots() } }
    }
    /// 手で選んだフォルダ（DCIM フォルダか、その親）
    private(set) var chosenFolder: URL?
    /// 読み込み先の空き容量（カードの大きさと比べて、足りなければ警告する）
    private(set) var freeSpace: Int64?
    private(set) var summary: CardSummary?
    private(set) var isSummarizing = false

    /// コピー先（ライブラリのルート）。<destination>/YYYY/YYYY-MM-DD/ に入る
    var destination: URL
    var verify = true
    var albumID: Int64?
    /// 取り込んだ写真に重ねる現像のプリセット（nil は「なし」）。選んだものを覚える（「なし」も覚える）。
    /// まだ選んだことがなければ、同梱の「Focal Default」（あれば）
    var presetID: String? {
        didSet {
            guard persistPreset, presetID != oldValue else { return }
            UserDefaults.standard.set(presetID ?? "", forKey: AppPaths.importPresetKey)
        }
    }
    private var persistPreset = false
    static let defaultPresetID = "builtin:Focal Default"
    /// "旅行/北海道, 2026" のようにカンマで区切る
    var tagsText = ""

    private(set) var phase: Phase = .settings
    private(set) var progressPhase: Catalog.CardImportEvent.Phase = .reading
    private(set) var done = 0
    private(set) var total = 0
    private(set) var bytesDone: Int64 = 0
    private(set) var bytesTotal: Int64 = 0
    private(set) var current = ""
    private(set) var result: CardImportResult?
    private(set) var failure: String?

    // MARK: 取り込む写真の選択（v3.29）

    /// カードの 1 枚ごとの一覧（撮影日時順）。取り込み済みの判定は core が決める
    private(set) var shots: [CardShot] = []
    private(set) var isListing = false
    private(set) var listProgress: (done: Int, total: Int) = (0, 0)
    private(set) var listFailure: String?
    /// チェックを外した写真の鍵。一覧を読み直しても（読み込み先を変えたときなど）外したままにする。新しく見つかった写真は
    /// チェックされている
    private(set) var unchecked: Set<String> = []
    /// 取り込み済みの写真を一覧から隠す（既定）
    var hideImported = true
    /// カード上のサムネイル（メモリ上のキャッシュ。ディスクには、サムネイルのキャッシュのフォルダに置く）
    let thumbnails = CardThumbnailLoader()
    private var listTask: Task<Void, Never>?

    var visibleShots: [CardShot] { hideImported ? shots.filter { !$0.imported } : shots }
    func isChecked(_ shot: CardShot) -> Bool { !shot.imported && !unchecked.contains(shot.key) }
    var checkedShots: [CardShot] { shots.filter(isChecked) }
    var checkedBytes: Int64 { checkedShots.reduce(0) { $0 + $1.bytes } }
    var importedCount: Int { shots.filter(\.imported).count }
    /// チェックできる（取り込み済みでない）写真の数
    var selectableCount: Int { shots.count - importedCount }
    /// 取り込みを始められる。一覧を読んでいる間は待つ。読めたらチェックした写真が 1 枚以上、読めなかったら概算の枚数が 1 枚以上
    var canStart: Bool {
        guard sourceURL != nil, !isListing else { return false }
        return hasList ? !checkedShots.isEmpty : (summary?.shots ?? 1) > 0
    }
    /// コピーに要る大きさ（チェックした写真。一覧を読めなかったらカードの全部）
    var neededBytes: Int64? { hasList ? checkedBytes : summary?.bytes }
    /// 一覧を読めた（取り込みは、チェックした写真だけを `only` で指定する）
    var hasList: Bool { !isListing && listFailure == nil && sourceURL != nil }

    func setChecked(_ keys: [String], _ on: Bool) {
        if on { unchecked.subtract(keys) } else { unchecked.formUnion(keys) }
    }

    func checkAll() { setChecked(visibleShots.filter { !$0.imported }.map(\.key), true) }
    func uncheckAll() { setChecked(visibleShots.filter { !$0.imported }.map(\.key), false) }

    /// カードの一覧を（読み込み先の取り込み済みの判定つきで）読み直す。遅いカードがあるので、core のスレッドで
    func loadShots() {
        listTask?.cancel()
        thumbnails.reset(cacheDirectory: AppPaths.thumbnailCache)
        shots = []
        listFailure = nil
        listProgress = (0, 0)
        guard let source = sourceURL, let model else {
            isListing = false
            return
        }
        isListing = true
        let catalog = model.catalog
        let dest = destination
        listTask = Task { @MainActor in
            do {
                for try await ev in catalog.listCardShots(source: source, destination: dest) {
                    switch ev {
                    case .progress(let d, let t): listProgress = (d, t)
                    case .finished(let list): shots = list
                    }
                }
            } catch {
                if !Task.isCancelled { listFailure = String(describing: error) }
            }
            if !Task.isCancelled { isListing = false }
        }
    }

    private weak var model: LibraryModel?
    private var task: Task<Void, Never>?
    private var summaryTask: Task<Void, Never>?

    init() {
        let env = ProcessInfo.processInfo.environment
        // UI テスト用: 取り込み元と読み込み先を環境変数で指定する
        // （カタログごとの読み込み先は、configure でカタログの設定から読む）
        destination = env["FOCAL_IMPORT_DEST"].map { URL(fileURLWithPath: $0) } ?? AppPaths.fallbackImportDestination
        chosenFolder = env["FOCAL_IMPORT_SOURCE"].map { URL(fileURLWithPath: $0) }
        if env["FOCAL_IMPORT_DEST"] == nil {
            // UI テスト（FOCAL_IMPORT_DEST 指定）では、記録を読み書きせず「なし」から始める
            if let saved = UserDefaults.standard.string(forKey: AppPaths.importPresetKey) {
                presetID = saved.isEmpty ? nil : saved
            } else {
                presetID = Self.defaultPresetID
            }
            persistPreset = true
        }
    }

    func configure(model: LibraryModel) {
        self.model = model
        guard ProcessInfo.processInfo.environment["FOCAL_IMPORT_DEST"] == nil else { return }
        if let saved = model.prefs.importDestination {
            destination = saved  // このカタログの設定
        } else if let first = model.roots.first(where: \.isOnline) {
            // 設定がなければ、すでにつながっているルートを既定にする
            destination = URL(fileURLWithPath: first.path)
            model.reloadSidebar()
        }
    }

    /// シートを開くとき
    func prepare(sources: [ImportSource], preferred: ImportSource?) {
        self.sources = sources
        if phase != .running {
            phase = .settings
            result = nil
            failure = nil
        }
        // 前回のプリセットが消えていたら「なし」に戻す
        if let id = presetID, model?.presets.list.contains(where: { $0.id == id }) != true {
            let keep = persistPreset
            persistPreset = false  // 一時的に見つからないだけかもしれないので、記録は変えない
            presetID = nil
            persistPreset = keep
        }
        if let preferred { selectedSourceID = preferred.id }
        else if selectedSourceID == nil || !sources.contains(where: { $0.id == selectedSourceID }) {
            if chosenFolder == nil { selectedSourceID = sources.first?.id }
        }
        summarize()
        loadShots()
        refreshFreeSpace()
    }

    /// カードの抜き差しで一覧が変わった
    func updateSources(_ list: [ImportSource]) {
        sources = list
        if let id = selectedSourceID, !list.contains(where: { $0.id == id }) { selectedSourceID = list.first?.id }
        if selectedSourceID == nil, chosenFolder == nil { selectedSourceID = list.first?.id }
    }

    var sourceURL: URL? {
        if let id = selectedSourceID, let s = sources.first(where: { $0.id == id }) { return URL(fileURLWithPath: s.mountPoint) }
        return chosenFolder
    }

    func chooseFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.prompt = String(localized: "Choose")
        panel.message = String(localized: "Choose the card or its DCIM folder.")
        if panel.runModal() == .OK, let url = panel.url {
            chosenFolder = url
            selectedSourceID = nil
            summarize()
            loadShots()
        }
    }

    func chooseDestination() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.directoryURL = destination
        panel.prompt = String(localized: "Choose")
        if panel.runModal() == .OK, let url = panel.url { setDestination(url) }
    }

    func refreshFreeSpace() {
        let dest = destination
        Task {
            let n = await Task.detached { Catalog.freeSpace(at: dest) }.value
            if n != freeSpace { freeSpace = n }
        }
    }

    /// 選んだカードを取り出す（macOS の取り出し）。カードの写真は消えない
    func ejectSelected() {
        guard let url = sourceURL, selectedSourceID != nil else { return }
        do {
            try NSWorkspace.shared.unmountAndEjectDevice(at: url)
            selectedSourceID = nil
            model?.refreshImportSources()
        } catch {
            failure = error.localizedDescription
        }
    }

    var canEject: Bool { selectedSourceID != nil && phase != .running }

    /// 取り込みが終わったあとに 1 回呼ぶ（終了を待つ用）
    var afterFinish: (() -> Void)?

    var isRunning: Bool { phase == .running }

    /// 読み込み先のフォルダがカタログから外れたとき。残っているフォルダの先頭へ移す。なければ既定の場所（設定の記録も消す）
    func moveDestination(toFirstOf roots: [RootEntry]) {
        if let first = roots.first {
            setDestination(URL(fileURLWithPath: first.path))
        } else {
            destination = AppPaths.fallbackImportDestination
            model?.prefs.setImportDestination(nil)  // このカタログの設定を消す
            refreshFreeSpace()
            model?.reloadSidebar()
        }
    }

    func setDestination(_ url: URL) {
        destination = url
        loadShots()  // 取り込み済みの判定は、読み込み先のファイルも見る
        refreshFreeSpace()
        persistDestination()
        model?.reloadSidebar()
    }

    /// 読み込み先を、このカタログの設定に記録する（UI テストで環境変数で指定したときは、記録しない）
    private func persistDestination() {
        model?.prefs.setImportDestination(destination)
    }

    /// 枚数と大きさの概算（遅いカードがあるので別のスレッドで）
    private func summarize() {
        summaryTask?.cancel()
        summary = nil
        guard let url = sourceURL else {
            isSummarizing = false
            return
        }
        isSummarizing = true
        summaryTask = Task {
            let s = await Task.detached { try? Catalog.summarizeCard(url) }.value
            guard !Task.isCancelled else { return }
            summary = s
            isSummarizing = false
        }
    }

    func start() {
        guard let model, let source = sourceURL, phase != .running else { return }
        var options = CardImportOptions(source: source, destination: destination)
        options.verify = verify
        options.albumID = albumID
        if let presetID, let store = model.presets.store {
            options.presetID = presetID
            options.presets = store
        }
        options.thumbnailCache = AppPaths.thumbnailCache
        if hasList { options.only = checkedShots.map(\.key) }  // 一覧を読めなかったときは、カードの全部（取り込み済みは除く）
        do {
            options.tagIDs = try tagsText.split(separator: ",")
                .map { $0.trimmingCharacters(in: .whitespaces) }
                .filter { !$0.isEmpty }
                .map { try model.catalog.ensureTag($0) }
        } catch {
            failure = String(describing: error)
            return
        }
        persistDestination()
        phase = .running
        progressPhase = .reading
        done = 0
        total = 0
        bytesDone = 0
        bytesTotal = 0
        current = ""
        result = nil
        failure = nil
        let catalog = model.catalog
        task = Task { @MainActor in
            do {
                for try await ev in catalog.importFromCard(options) {
                    switch ev {
                    case .progress(let p, let d, let t, let bd, let bt, let name):
                        progressPhase = p
                        done = d
                        total = t
                        bytesDone = bd
                        bytesTotal = bt
                        if !name.isEmpty { current = name }
                    case .finished(let r):
                        result = r
                    }
                }
            } catch {
                failure = String(describing: error)
            }
            phase = .finished
            model.reloadSidebar()
            model.cardImportFinished(showRecent: false)
            afterFinish?()
            afterFinish = nil
        }
    }

    func cancel() { task?.cancel() }

    /// 完了後に「前回の読み込み」を表示する
    func showImported() {
        model?.cardImportFinished(showRecent: true)
    }
}

/// カード上の写真のサムネイル（取り込み前なので、カタログには結びつけない）。カードを読むので同時に数本までにする
final class CardThumbnailLoader: @unchecked Sendable {
    private let cache = NSCache<NSString, CGImage>()
    private let gate = ConcurrencyGate(limit: 3)
    private let lock = NSLock()
    private var directory: URL?

    init() { cache.countLimit = 3000 }

    func reset(cacheDirectory: URL) {
        lock.lock()
        directory = cacheDirectory
        lock.unlock()
        cache.removeAllObjects()
    }

    private func currentDirectory() -> URL? {
        lock.lock()
        defer { lock.unlock() }
        return directory
    }

    func cached(_ shot: CardShot) -> CGImage? { cache.object(forKey: shot.key as NSString) }

    /// Task がキャンセルされたら（セルが画面から出たら）、開始前の読み込みはやめる
    func image(for shot: CardShot) async -> CGImage? {
        if let img = cached(shot) { return img }
        guard shot.isPhoto else { return nil }
        guard let dir = currentDirectory() else { return nil }
        await gate.enter()
        var image: CGImage?
        if !Task.isCancelled {
            image = await Task.detached(priority: .utility) { () -> CGImage? in
                guard let url = try? Catalog.cardThumbnail(file: shot.path, cacheDirectory: dir) else { return nil }
                return ThumbnailLoader.decode(url)
            }.value
        }
        await gate.leave()
        if let image { cache.setObject(image, forKey: shot.key as NSString) }
        return image
    }
}

/// 同時に動かす数を絞る（待つ側は、開始前なら動かさずに抜けられる）
actor ConcurrencyGate {
    private var available: Int
    private var waiters: [CheckedContinuation<Void, Never>] = []

    init(limit: Int) { available = limit }

    func enter() async {
        if available > 0 {
            available -= 1
            return
        }
        await withCheckedContinuation { waiters.append($0) }
    }

    func leave() {
        if waiters.isEmpty {
            available += 1
        } else {
            waiters.removeFirst().resume()
        }
    }
}
