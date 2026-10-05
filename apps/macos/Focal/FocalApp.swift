import AppKit
import FocalCore
import SwiftUI
import UniformTypeIdentifiers

@main
struct FocalApp: App {
    @NSApplicationDelegateAdaptor private var delegate: AppDelegate
    @State private var state = AppState()

    init() {
        // UI テスト（FOCAL_WINDOW_SIZE 指定）では、前回の起動で保存された列とインスペクタの区分の開閉を使わない
        // （サイドバーや区分をたたんだ状態が残ると、以降のテストで要素が出ない）
        if ProcessInfo.processInfo.environment["FOCAL_WINDOW_SIZE"] != nil {
            let defaults = UserDefaults.standard
            for key in defaults.dictionaryRepresentation().keys where key.hasPrefix("NSSplitView Subview Frames") || key.hasPrefix("inspector.") || key.hasPrefix("sidebar.") || key.hasPrefix("filmstrip.") {
                defaults.removeObject(forKey: key)
            }
        }
    }

    var body: some Scene {
        Window("Focal", id: "library") {
            Group {
                if let model = state.model {
                    ContentView(model: model)
                        .id(ObjectIdentifier(model))  // カタログを切り替えたら作り直す（キー操作の監視なども）
                        .navigationSubtitle(model.windowSubtitle(catalogName: state.catalogName))
                } else {
                    WelcomeView(state: state)
                }
            }
            // サイドバー + グリッド + インスペクタが収まる大きさ。SwiftUI の .frame(minWidth:) にすると、更新のたびに
            // ウィンドウ全体（インスペクタの全行を含む）の最小サイズを測り直して重いので、ウィンドウに直接設定する
            .background(WindowMinSize(size: NSSize(width: 1180, height: 640)))
            .alert("Cannot Open Catalog",
                   isPresented: Binding(get: { state.model != nil && state.error != nil },
                                        set: { if !$0 { state.error = nil } })) {
                Button("OK") { state.error = nil }
            } message: {
                Text(state.error ?? "")
            }
            .onAppear {
                delegate.state = state
                if let url = delegate.pendingOpen {
                    delegate.pendingOpen = nil
                    state.open(url)
                }
                applyTestWindowSize()
            }
        }
        .defaultSize(width: 1440, height: 900)
        .commands { LibraryCommands(state: state, model: state.model) }

        Settings {
            SettingsView(state: state)
        }

        Window("Third-Party Licenses", id: "licenses") {
            LicensesView()
        }
        .defaultSize(width: 860, height: 600)
    }
}

/// UI テスト用: FOCAL_WINDOW_SIZE=幅x高さ（ポイント）でウィンドウの大きさを、
/// FOCAL_WINDOW_SCREEN=番号（0 が主ディスプレイ）で置くディスプレイを決める（8.2 章: ディスプレイごとの色の確認）
@MainActor
private func applyTestWindowSize() {
    let env = ProcessInfo.processInfo.environment
    let size = env["FOCAL_WINDOW_SIZE"]?.split(separator: "x").compactMap { Double($0) }
    let screenIndex = env["FOCAL_WINDOW_SCREEN"].flatMap(Int.init)
    guard size?.count == 2 || screenIndex != nil else { return }
    DispatchQueue.main.async {
        guard let window = NSApp.windows.first(where: { $0.isVisible }) else { return }
        var frame = window.frame
        if let size, size.count == 2 { frame.size = NSSize(width: size[0], height: size[1]) }
        if let i = screenIndex, NSScreen.screens.indices.contains(i) {
            let vf = NSScreen.screens[i].visibleFrame
            frame.size.width = min(frame.width, vf.width)
            frame.size.height = min(frame.height, vf.height)
            frame.origin = NSPoint(x: vf.midX - frame.width / 2, y: vf.midY - frame.height / 2)
        }
        window.setFrame(frame, display: true)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    @MainActor weak var state: AppState?

    /// カードの取り込み中に終了するときは、取り込みを止めて（コピー済みの分は登録され、書きかけのファイルは消える）、
    /// 終わるのを待ってから終了する。10 秒待っても終わらなければ終了する
    @MainActor
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let importer = state?.model?.cardImport, importer.isRunning else { return .terminateNow }
        importer.afterFinish = { NSApp.reply(toApplicationShouldTerminate: true) }
        importer.cancel()
        DispatchQueue.main.asyncAfter(deadline: .now() + 10) { NSApp.reply(toApplicationShouldTerminate: true) }
        return .terminateLater
    }

    @MainActor
    func applicationWillTerminate(_ notification: Notification) {
        state?.model?.prepareForTermination()
    }

    @MainActor
    func applicationWillFinishLaunching(_ notification: Notification) {
        AppAppearance.current.apply()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    /// Finder でカタログ（〜.focalcatalog）を開いた
    @MainActor
    func application(_ application: NSApplication, open urls: [URL]) {
        guard let url = urls.first(where: AppPaths.isPackage) else { return }
        if let state {
            state.open(url)
        } else {
            pendingOpen = url  // 起動直後（ウィンドウが出る前）
        }
    }

    @MainActor var pendingOpen: URL?
}

/// 開いているカタログ（初回起動・切り替え・前回のカタログの記録）
@MainActor
@Observable
final class AppState {
    private(set) var model: LibraryModel?
    private(set) var catalogURL: URL?
    /// 初回起動の画面に出すエラー
    var error: String?

    static let defaultPreviewCacheLimit: UInt64 = 2 << 30
    private static let lastCatalogKey = "LastCatalogPath"
    private static let previewCacheLimitKey = "PreviewCacheLimit"

    /// 大きいプレビューのキャッシュの上限（バイト。設定画面で変える）
    var previewCacheLimit: UInt64 {
        didSet {
            UserDefaults.standard.set(Int64(previewCacheLimit), forKey: Self.previewCacheLimitKey)
            try? model?.develop.editor.setPreviewCacheLimit(previewCacheLimit)
        }
    }

    var catalogName: String { catalogURL?.deletingPathExtension().lastPathComponent ?? "" }

    init() {
        let stored = UserDefaults.standard.object(forKey: Self.previewCacheLimitKey) as? Int64
        previewCacheLimit = stored.map { UInt64(max($0, 0)) } ?? Self.defaultPreviewCacheLimit
        if let url = Self.initialCatalog() { open(url) }
    }

    /// 起動時に開くカタログ。なければ nil（初回起動の画面を出す）
    private static func initialCatalog() -> URL? {
        if let url = AppPaths.fixedCatalog { return url }
        if AppPaths.isolatedFromDefaults { return nil }
        if let p = UserDefaults.standard.string(forKey: lastCatalogKey), !p.isEmpty {
            let url = URL(fileURLWithPath: p)
            if AppPaths.catalogExists(url) { return url }
        }
        // 0.2.0 までの保存先にあれば、それを使い続ける（データを失わない）
        if AppPaths.catalogExists(AppPaths.legacyCatalog) { return AppPaths.legacyCatalog }
        return nil
    }

    /// 既存のカタログを開く。開けなければ今のカタログのまま false
    @discardableResult
    func open(_ url: URL) -> Bool {
        guard AppPaths.catalogExists(url) || AppPaths.fixedCatalog == url else {
            error = String(localized: "“\(url.lastPathComponent)” is not a Focal catalog.")
            return false
        }
        return load(url)
    }

    /// 新しいカタログを作って開く
    @discardableResult
    func create(_ url: URL) -> Bool {
        do {
            if AppPaths.isPackage(url) {
                try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
            }
        } catch {
            self.error = error.localizedDescription
            return false
        }
        return load(url)
    }

    private func load(_ url: URL) -> Bool {
        if url.standardizedFileURL == catalogURL?.standardizedFileURL, model != nil { return true }
        let previous = model
        previous?.prepareForTermination()
        do {
            let next = try LibraryModel(catalogURL: AppPaths.database(of: url), cacheURL: AppPaths.thumbnailCache,
                                        previewCacheURL: AppPaths.previewCache, previewCacheLimit: previewCacheLimit)
            model = next
            catalogURL = url
            error = nil
            if !AppPaths.isolatedFromDefaults {
                UserDefaults.standard.set(url.path, forKey: Self.lastCatalogKey)
            }
            return true
        } catch {
            self.error = String(describing: error)
            return false
        }
    }

    // MARK: パネル

    func chooseNewCatalog() {
        let panel = NSSavePanel()
        panel.title = String(localized: "New Catalog")
        panel.prompt = String(localized: "Create")
        panel.allowedContentTypes = [AppPaths.catalogType]
        let suggested = AppPaths.defaultNewCatalog
        try? FileManager.default.createDirectory(at: suggested.deletingLastPathComponent(),
                                                 withIntermediateDirectories: true)
        panel.directoryURL = suggested.deletingLastPathComponent()
        panel.nameFieldStringValue = suggested.deletingPathExtension().lastPathComponent
        guard panel.runModal() == .OK, var url = panel.url else { return }
        if !AppPaths.isPackage(url) { url.appendPathExtension(AppPaths.catalogExtension) }
        if AppPaths.catalogExists(url) {
            open(url)  // 既存のカタログを選んだ（置き換えはしない）
        } else {
            create(url)
        }
    }

    func chooseExistingCatalog() {
        let panel = NSOpenPanel()
        panel.title = String(localized: "Open Catalog")
        panel.prompt = String(localized: "Open")
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        panel.allowedContentTypes = [AppPaths.catalogType, UTType(filenameExtension: "sqlite") ?? .data]
        panel.directoryURL = catalogURL?.deletingLastPathComponent() ?? AppPaths.defaultCatalogDirectory
        if panel.runModal() == .OK, let url = panel.url { open(url) }
    }
}

/// メニューと修飾キー付きのショートカット（9.2 章）。単キーはビュー側で受ける
struct LibraryCommands: Commands {
    let state: AppState
    let model: LibraryModel?
    @Environment(\.openWindow) private var openWindow

    var body: some Commands {
        // 「Focal について」: 版に、β などのリリースの種類を添える（Info.plist の FocalReleaseChannel）
        CommandGroup(replacing: .appInfo) {
            Button("About Focal") {
                NSApp.orderFrontStandardAboutPanel(options: [.applicationVersion: AppInfo.displayVersion])
            }
        }
        CommandGroup(after: .help) {
            Button("Third-Party Licenses") { openWindow(id: "licenses") }
        }
        CommandGroup(after: .newItem) {
            Button("New Catalog…") { state.chooseNewCatalog() }
            Button("Open Catalog…") { state.chooseExistingCatalog() }
                .keyboardShortcut("o", modifiers: [.command, .option])
            Button("New Album…") { model?.albumPrompt = .create(addSelection: false) }
                .keyboardShortcut("n", modifiers: [.command, .shift])
                .disabled(model == nil)
            Button("New Smart Album…") { model?.albumPrompt = .smart(editing: nil) }
                .keyboardShortcut("n", modifiers: [.command, .option])
                .disabled(model == nil)
            Button("New Folder…") { model?.albumPrompt = .createFolder() }
                .disabled(model == nil)
            Divider()
            Button("Import from Card…") { model?.beginCardImport() }
                .keyboardShortcut("i", modifiers: [.command, .shift])
                .disabled(model == nil)
            Button("Add Folder…") { if let model { chooseFolderToAdd(model: model) } }
                .keyboardShortcut("o")
                .disabled(model == nil)
            Button("Rescan All Folders") { model?.rescanAll() }
                .keyboardShortcut("r", modifiers: [.command, .shift])
                .disabled(model == nil)
            Divider()
            Button("Export…") { model?.beginExport() }
                .keyboardShortcut("e", modifiers: [.command, .shift])
                .disabled(model == nil)
        }
        CommandGroup(replacing: .undoRedo) {
            // 現像の Undo は core の Undo スタック（9.3 章）。テキスト入力中はテキストの Undo に渡す
            Button("Undo") {
                if model?.develop.isEditingText == true || model?.mode != .viewer {
                    NSApp.sendAction(Selector(("undo:")), to: nil, from: nil)
                } else {
                    model?.develop.undo()
                }
            }
            .keyboardShortcut("z")
            .disabled(!(model?.develop.canUndo ?? false) && model?.develop.isEditingText != true)
            Button("Redo") {
                if model?.develop.isEditingText == true || model?.mode != .viewer {
                    NSApp.sendAction(Selector(("redo:")), to: nil, from: nil)
                } else {
                    model?.develop.redo()
                }
            }
            .keyboardShortcut("z", modifiers: [.command, .shift])
            .disabled(!(model?.develop.canRedo ?? false) && model?.develop.isEditingText != true)
        }
        CommandMenu("Photo") {
            ForEach(0...5, id: \.self) { r in
                Button(r == 0 ? String(localized: "No Rating") : String(repeating: "★", count: r)) { model?.setRating(r) }
            }
            Divider()
            Button("Crop") { model?.develop.toggleCropMode() }
                .disabled(model?.mode != .viewer)
            Button("Rotate Left") { model?.develop.rotate(by: -1) }
                .disabled(model?.mode != .viewer)
            Button("Rotate Right") { model?.develop.rotate(by: 1) }
                .disabled(model?.mode != .viewer)
            Divider()
            // アルバム（v3.16）
            Menu("Add to Album") {
                // 写真を足せるのは手で集めるアルバムだけ（フォルダ・スマートアルバムは出さない）
                ForEach((model?.albums ?? []).filter(\.acceptsPhotos)) { album in
                    Button(album.name) { model?.addToAlbum(album.id) }
                }
                if !(model?.albums.filter(\.acceptsPhotos).isEmpty ?? true) { Divider() }
                Button("New Album with Selected Photos…") { model?.albumPrompt = .create(addSelection: true) }
            }
            .disabled(model == nil)
            Menu("Apply Preset") {
                if let presets = model?.presets {
                    PresetMenuItems(presets: presets, canSave: model?.mode == .viewer && model?.develop.stage == .ready)
                }
            }
            .disabled(model == nil)
            Button("Use as Album Cover") { model?.setCurrentAlbumCover() }
                .disabled(!(model?.canSetAlbumCover ?? false))
            // 写真をディスクから削除する。⌥⌃ を押しながらメニューを開いたときだけ有効（キーは ⌥⌃⇧ Delete）
            Button("Move to Trash…") { model?.requestDelete() }
                .disabled(!(model?.deleteArmed ?? false))
            Button("Remove from Album") { model?.removeFromCurrentAlbum() }
                .disabled(!(model?.currentAlbumAcceptsPhotos ?? false))
            Divider()
            Button("Pick") { model?.setFlag(.picked) }
            Button("Reject") { model?.setFlag(.rejected) }
            Button("Unflag") { model?.setFlag(.none) }
        }
        CommandGroup(after: .sidebar) {
            Button(LocalizedStringResource("mode.library", defaultValue: "Library")) { model?.mode = .grid }
                .keyboardShortcut("1")
            Button(LocalizedStringResource("mode.develop", defaultValue: "Develop")) { model?.mode = .viewer }
                .keyboardShortcut("2")
            Button("Toggle Library / Develop") { model?.toggleMode() }
            Button("Show / Hide Filter Bar") { model?.showFilterBar.toggle() }
                .keyboardShortcut("f", modifiers: [.command, .option])
            Button("Show / Hide Filmstrip") {
                UserDefaults.standard.set(!UserDefaults.standard.bool(forKey: "filmstrip.visible", default: true),
                                          forKey: "filmstrip.visible")
            }
            .keyboardShortcut("b", modifiers: [.command, .option])
            Button("Toggle Fit / 100%") { model?.develop.toggleZoom(at: nil) }
                .disabled(model?.mode != .viewer)
            Button("Toggle Inspector") { model?.showInspector.toggle() }
                .keyboardShortcut("i", modifiers: [.command, .option])
        }
    }
}

/// フォルダを選んでカタログに追加する（ファイルメニューと空のカタログの画面）
@MainActor
func chooseFolderToAdd(model: LibraryModel) {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = true
    panel.canChooseFiles = false
    panel.allowsMultipleSelection = false
    panel.prompt = String(localized: "Add")
    if panel.runModal() == .OK, let url = panel.url { model.importFolder(url) }
}

/// ウィンドウの最小サイズを AppKit で設定する（SwiftUI の .frame(minWidth:) の代わり）
private struct WindowMinSize: NSViewRepresentable {
    let size: NSSize

    func makeNSView(context: Context) -> MinSizeView {
        let v = MinSizeView()
        v.minSize = size
        return v
    }

    func updateNSView(_ v: MinSizeView, context: Context) {
        v.minSize = size
        v.apply()
    }

    final class MinSizeView: NSView {
        var minSize = NSSize.zero

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            apply()
        }

        func apply() {
            guard let window, window.contentMinSize != minSize else { return }
            window.contentMinSize = minSize
        }
    }
}

extension UserDefaults {
    /// 値がなければ既定値
    func bool(forKey key: String, default value: Bool) -> Bool {
        object(forKey: key) == nil ? value : bool(forKey: key)
    }
}

/// アプリの版の表示（26.0.0 β のように、版にリリースの種類を添える。種類が空なら版だけ）
enum AppInfo {
    static var displayVersion: String {
        let info = Bundle.main.infoDictionary ?? [:]
        let version = info["CFBundleShortVersionString"] as? String ?? ""
        let channel = (info["FocalReleaseChannel"] as? String ?? "").trimmingCharacters(in: .whitespaces)
        return channel.isEmpty ? version : "\(version) \(channel)"
    }
}
