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
        didSet { if selectedSourceID != oldValue { summarize() } }
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

    private weak var model: LibraryModel?
    private var task: Task<Void, Never>?
    private var summaryTask: Task<Void, Never>?

    init() {
        let env = ProcessInfo.processInfo.environment
        // UI テスト用: 取り込み元と読み込み先を環境変数で指定する
        destination = env["FOCAL_IMPORT_DEST"].map { URL(fileURLWithPath: $0) }
            ?? AppPaths.savedImportDestination ?? AppPaths.fallbackImportDestination
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
        // 設定がなければ、すでにつながっているルートを既定にする
        if AppPaths.savedImportDestination == nil, ProcessInfo.processInfo.environment["FOCAL_IMPORT_DEST"] == nil,
           let first = model.roots.first(where: \.isOnline) {
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
            if ProcessInfo.processInfo.environment["FOCAL_IMPORT_DEST"] == nil {
                UserDefaults.standard.removeObject(forKey: AppPaths.importDestinationKey)
            }
            refreshFreeSpace()
            model?.reloadSidebar()
        }
    }

    func setDestination(_ url: URL) {
        destination = url
        refreshFreeSpace()
        persistDestination()
        model?.reloadSidebar()
    }

    /// 読み込み先を設定に記録する（UI テストで環境変数で指定したときは、実際の設定を変えない）
    private func persistDestination() {
        guard ProcessInfo.processInfo.environment["FOCAL_IMPORT_DEST"] == nil else { return }
        UserDefaults.standard.set(destination.path, forKey: AppPaths.importDestinationKey)
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
