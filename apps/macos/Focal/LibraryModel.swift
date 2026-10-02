import AppKit
import Foundation
import FocalCore
import Observation

/// フォルダツリーの節（サイドバー用）
struct FolderNode: Identifiable, Hashable {
    let id: Int64
    let name: String
    let photoCount: Int64
    var children: [FolderNode]?
}

/// タグツリーの節
struct TagNode: Identifiable, Hashable {
    let id: Int64
    let name: String
    let photoCount: Int64
    var children: [TagNode]?
}

/// ストレージの 1 つのルート（ライブラリのルートフォルダ）とそのフォルダツリー（v3.19）
struct RootEntry: Identifiable, Hashable {
    let id: Int64
    let path: String
    let node: FolderNode
    let isOnline: Bool
    /// 読み込み先（設定の既定のコピー先）になっている
    let isImportDestination: Bool
}

/// ストレージ（ボリューム）の節。外付けドライブが外れていると isOnline = false（グレー表示）
struct VolumeGroup: Identifiable, Hashable {
    let id: String
    let name: String
    let isOnline: Bool
    var roots: [RootEntry]
}

/// アルバムのツリーの節（フォルダなら children を持つ）
struct AlbumNode: Identifiable, Hashable {
    let album: Album
    var children: [AlbumNode]?
    var id: Int64 { album.id }
}

enum MainMode: String { case grid, viewer }

/// ライブラリ画面の状態（9 章）。画像処理・カタログのロジックは持たず、FocalCore を呼ぶだけ（ADR-10）。
@MainActor
@Observable
final class LibraryModel {
    // MARK: 状態

    private(set) var roots: [PhotoRoot] = []
    private(set) var folderTree: [FolderNode] = []
    /// ストレージ（ボリューム）ごとのルートとフォルダ（v3.19）
    private(set) var volumes: [VolumeGroup] = []
    private(set) var tagTree: [TagNode] = []
    /// すべてのタグ（"親/子" のパスつき。スマートアルバムの条件の選択肢）
    private(set) var tags: [PhotoTag] = []
    /// アルバム（v3.16）。親が先、同じ親の中は作った順（v3.19 でフォルダ・スマートアルバムを含む）
    private(set) var albums: [Album] = []
    private(set) var albumTree: [AlbumNode] = []
    /// いま接続されている DCIM のあるボリューム（SD カードなど。v3.19）
    private(set) var importSources: [ImportSource] = []

    var filter = PhotoFilter() { didSet { if filter != oldValue { reloadPhotos() } } }

    /// 絞り込み結果の id（グリッドの並び）。行データは photo(at:) で必要な分だけ読む
    private(set) var photoIDs: [Int64] = []
    /// グリッドを作り直すべき変更の世代
    private(set) var listGeneration = 0
    /// ★やフラグが変わった写真（セルの表示更新用）
    private(set) var changedPhotos: (generation: Int, ids: Set<Int64>) = (0, [])

    var currentIndex: Int? = nil {
        didSet { if currentIndex != oldValue { syncDevelop() } }
    }
    var selection: Set<Int64> = []
    var mode: MainMode = .grid {
        didSet { if mode != oldValue { syncDevelop() } }
    }
    var thumbnailSize: Double = 160
    var showInspector = true
    /// 絞り込みバー（9.5 章）を出しているか
    var showFilterBar = false
    var showExport = false
    let export = ExportModel()
    /// カードの取り込みのシート（v3.19）
    var showImport = false
    let cardImport = CardImportModel()

    private(set) var importProgress: (done: Int, total: Int)? = nil
    private(set) var lastError: String? = nil

    var currentPhoto: Photo? { currentIndex.flatMap { photo(at: $0) } }

    // MARK: core

    let catalog: Catalog
    let thumbnails: ThumbnailLoader
    let develop: DevelopModel
    private var pageCache: [Int: [Photo]] = [:]  // ページ番号 → 写真
    private static let pageSize = 200

    init(catalogURL: URL, cacheURL: URL, previewCacheURL: URL, previewCacheLimit: UInt64) throws {
        try FileManager.default.createDirectory(at: catalogURL.deletingLastPathComponent(),
                                                withIntermediateDirectories: true)
        catalog = try Catalog(url: catalogURL)
        thumbnails = ThumbnailLoader(thumbnailer: try Thumbnailer(catalog: catalog, cacheDirectory: cacheURL))
        develop = try DevelopModel(catalog: catalog)
        reloadSidebar()
        try develop.setThumbnailCache(cacheURL) { [weak self] id in self?.thumbnailUpdated(id) }
        try develop.editor.setPreviewCache(previewCacheURL, limitBytes: previewCacheLimit)
        cardImport.configure(model: self)
        reloadPhotos()
        observeVolumes()
        refreshImportSources()
        if !AppPaths.isolatedFromDefaults && UserDefaults.standard.bool(forKey: AppPaths.rescanOnLaunchKey, default: true) {
            rescanAll()  // 起動時の再スキャン（外でフォルダやファイルが変わっていた場合に追従する）
        }
    }

    // MARK: 読み込み

    func reloadSidebar() {
        do {
            roots = try catalog.roots()
            folderTree = try roots.map { root in
                let folders = try catalog.folders(rootID: root.id)
                return Self.buildFolderTree(root: root, folders: folders)
            }
            tags = try catalog.tags()
            tagTree = Self.buildTagTree(tags)
            albums = try catalog.albums()
            albumTree = Self.buildAlbumTree(albums)
            volumes = Self.buildVolumes(roots: roots, trees: folderTree, importDestination: cardImport.destination)
        } catch {
            report(error)
        }
    }

    func reloadPhotos() {
        do {
            let previous = currentIndex.flatMap { photoIDs.indices.contains($0) ? photoIDs[$0] : nil }
            photoIDs = try catalog.photoIDs(filter)
            pageCache.removeAll()
            listGeneration += 1
            if let previous, let i = photoIDs.firstIndex(of: previous) {
                currentIndex = i
            } else {
                currentIndex = photoIDs.isEmpty ? nil : 0
            }
            selection = currentIndex.map { [photoIDs[$0]] } ?? []
        } catch {
            report(error)
        }
    }

    /// index の写真。同じページの写真をまとめて読む
    func photo(at index: Int) -> Photo? {
        guard photoIDs.indices.contains(index) else { return nil }
        let page = index / Self.pageSize
        if pageCache[page] == nil {
            if pageCache.count >= 50 { pageCache.removeAll(keepingCapacity: true) }  // 1 万件分まで
            let start = page * Self.pageSize
            let ids = Array(photoIDs[start..<min(photoIDs.count, start + Self.pageSize)])
            pageCache[page] = (try? catalog.photos(ids: ids)) ?? []
        }
        let rows = pageCache[page]!
        let offset = index - page * Self.pageSize
        return rows.indices.contains(offset) && rows[offset].id == photoIDs[index] ? rows[offset]
            : rows.first { $0.id == photoIDs[index] }
    }

    // MARK: 選択と移動（9.2 章）

    func select(index: Int, extend: Bool = false) {
        guard photoIDs.indices.contains(index) else { return }
        currentIndex = index
        if extend { selection.insert(photoIDs[index]) } else { selection = [photoIDs[index]] }
    }

    func move(by delta: Int) {
        guard !photoIDs.isEmpty else { return }
        let i = max(0, min(photoIDs.count - 1, (currentIndex ?? 0) + delta))
        select(index: i)
    }

    func toggleMode() { mode = (mode == .grid) ? .viewer : .grid }

    /// ビューアでは選択中の写真を現像ビューアで開き、次の写真を先読みする（5.3 章）。グリッドに戻ったら閉じる
    private func syncDevelop() {
        guard mode == .viewer, let i = currentIndex, photoIDs.indices.contains(i) else {
            develop.close()
            return
        }
        let next = photoIDs.indices.contains(i + 1) ? photoIDs[i + 1] : nil
        develop.open(photoID: photoIDs[i], next: next, placeholder: thumbnails.cached(photoIDs[i]))
    }

    // MARK: 書き出し（5.8 章）

    /// 書き出す写真（選択中。なければ表示中の写真）を撮影日時順で
    var exportIDs: [Int64] {
        let selected = selection.isEmpty ? Set(currentIndex.map { [photoIDs[$0]] } ?? []) : selection
        return photoIDs.filter { selected.contains($0) }
    }

    func beginExport() {
        guard !exportIDs.isEmpty else { return }
        export.reset()
        showExport = true
    }

    func startExport() {
        develop.saveNow()  // 現像中の編集を書き出しに反映する
        export.start(catalog: catalog, ids: exportIDs)
    }

    /// アプリの終了時: 編集を保存し、書き込みが終わるまで待つ（7.4 章）
    func prepareForTermination() {
        stopObservingVolumes()
        watcher.stop()
        develop.close()
        try? catalog.flush()
    }

    /// メニューの操作の対象（選択中の写真。なければ今の写真）
    var targetIDs: [Int64] {
        if !selection.isEmpty { return Array(selection) }
        return currentIndex.map { [photoIDs[$0]] } ?? []
    }

    // MARK: ★・フラグ・タグ（Undo の対象外、9.3 章）

    func setRating(_ rating: Int) {
        let ids = targetIDs
        guard !ids.isEmpty else { return }
        do {
            try catalog.setRating(rating, for: ids)
            afterPhotoChange(ids)
        } catch { report(error) }
    }

    func setFlag(_ flag: PhotoFlag) {
        let ids = targetIDs
        guard !ids.isEmpty else { return }
        do {
            try catalog.setFlag(flag, for: ids)
            afterPhotoChange(ids)
        } catch { report(error) }
    }

    func addTag(path: String) {
        let ids = targetIDs
        let trimmed = path.trimmingCharacters(in: .whitespaces)
        guard !ids.isEmpty, !trimmed.isEmpty else { return }
        do {
            let tag = try catalog.ensureTag(trimmed)
            try catalog.addTag(tag, to: ids)
            reloadSidebar()
            afterPhotoChange(ids)
        } catch { report(error) }
    }

    func removeTag(_ tagID: Int64) {
        let ids = targetIDs
        do {
            try catalog.removeTag(tagID, from: ids)
            reloadSidebar()
            afterPhotoChange(ids)
        } catch { report(error) }
    }

    func tags(ofPhoto id: Int64) -> [PhotoTag] { (try? catalog.tags(ofPhoto: id)) ?? [] }

    /// 編集後のサムネイルができた（10 章）: メモリのキャッシュを捨ててセルを読み直す
    private(set) var reloadedThumbnails: (generation: Int, ids: Set<Int64>) = (0, [])

    private func thumbnailUpdated(_ id: Int64) {
        thumbnails.invalidate(id)
        reloadedThumbnails = (reloadedThumbnails.generation + 1, [id])
    }

    private func afterPhotoChange(_ ids: [Int64]) {
        pageCache.removeAll()
        // 絞り込みの条件に関わる変更なら並びを作り直す
        if albums.contains(where: { $0.kind == .smart }) { reloadSidebar() }  // スマートアルバムの枚数
        if filter.minRating > 0 || filter.flag != .any || filter.tagID != nil || filter.smartAlbumID != nil {
            reloadPhotos()
        } else {
            changedPhotos = (changedPhotos.generation + 1, Set(ids))
        }
    }

    // MARK: 取り込み

    func importFolder(_ url: URL) {
        Task { await scan(addingRoot: url) }
    }

    /// 登録したフォルダをすべて再スキャンする。ドライブが外れているルートは飛ばす
    func rescanAll() {
        Task {
            for root in roots where root.isOnline { await scan(rootID: root.id) }
        }
    }

    /// 外す確認を出すルート（SidebarView の確認ダイアログ）
    var rootToRemove: RootEntry?

    /// ルートをカタログから外す（ファイルは消えない）。見ていたフォルダなら「すべての写真」に戻す
    func removeRoot(_ root: RootEntry) {
        do {
            if case .folder(let id) = source, Self.find(id, in: [root.node]) != nil { source = .all }
            try catalog.removeRoot(root.id)
            reloadSidebar()
            reloadPhotos()
            updateWatcher()
        } catch { report(error) }
    }

    func rescan(rootID: Int64) {
        Task { await scan(rootID: rootID) }
    }

    private func scan(addingRoot url: URL) async {
        do {
            let id = try catalog.addRoot(url)
            reloadSidebar()
            await scan(rootID: id)
        } catch { report(error) }
    }

    /// 同時に走らせるのは 1 つだけ（同じルートを 2 つのスキャンが同時に書くと重複するため）
    private var scanQueue: [Int64] = []
    private(set) var isScanning = false

    private func scan(rootID: Int64) async {
        if isScanning || cardImport.phase == .running {
            if !scanQueue.contains(rootID) { scanQueue.append(rootID) }
            return
        }
        isScanning = true
        importProgress = (0, 0)
        defer {
            importProgress = nil
            isScanning = false
        }
        do {
            // サムネイルは表示時に作る（取り込みを早く終わらせる）
            for try await ev in catalog.scan(rootID: rootID, thumbnailCache: nil) {
                if case .progress(let done, let total) = ev { importProgress = (done, total) }
            }
        } catch { report(error) }
        reloadSidebar()
        reloadPhotos()
        if !scanQueue.isEmpty {
            let next = scanQueue.removeFirst()
            await scan(rootID: next)
        }
    }

    // MARK: ボリューム・フォルダの監視（v3.19）

    private var volumeObservers: [NSObjectProtocol] = []
    private let watcher = FolderWatcher()

    private func observeVolumes() {
        let center = NSWorkspace.shared.notificationCenter
        for name in [NSWorkspace.didMountNotification, NSWorkspace.didUnmountNotification] {
            volumeObservers.append(center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                Task { @MainActor in self?.volumesChanged() }
            })
        }
    }

    private func stopObservingVolumes() {
        for o in volumeObservers { NSWorkspace.shared.notificationCenter.removeObserver(o) }
        volumeObservers = []
    }

    /// ドライブがつながった・外れた: ルートの場所をいまのマウントポイントに合わせ、サイドバーを更新する
    private func volumesChanged() {
        let catalog = catalog
        Task {
            _ = await Task.detached { try? catalog.refreshVolumes() }.value
            reloadSidebar()
            refreshImportSources()
            updateWatcher()
        }
    }

    func refreshImportSources() {
        Task {
            let sources = await Task.detached { (try? Catalog.importSources()) ?? [] }.value
            if sources != importSources {
                importSources = sources
                cardImport.updateSources(sources)
            }
        }
    }

    /// 表示中のフォルダだけ監視する（FSEvents。全体の常時監視はしない）。変わったらそのルートを再スキャンする
    private func updateWatcher() {
        guard case .folder(let id) = source, let entry = rootEntry(containingFolder: id), entry.isOnline else {
            watcher.stop()
            return
        }
        watcher.watch(path: entry.path) { [weak self] in
            guard let self, !self.isScanning, self.cardImport.phase != .running else { return }
            self.rescan(rootID: entry.id)
        }
    }

    private func rootEntry(containingFolder id: Int64) -> RootEntry? {
        for v in volumes {
            for r in v.roots where Self.find(id, in: [r.node]) != nil { return r }
        }
        return nil
    }

    // MARK: カードの取り込み（v3.19）

    func beginCardImport(source: ImportSource? = nil) {
        cardImport.prepare(sources: importSources, preferred: source)
        showImport = true
    }

    /// 取り込みが終わった（コピーした分は登録済み）: 画面を更新する
    func cardImportFinished(showRecent: Bool) {
        reloadSidebar()
        if showRecent {
            source = .recentImport
        } else {
            reloadPhotos()
        }
        if !scanQueue.isEmpty {
            let next = scanQueue.removeFirst()
            Task { await scan(rootID: next) }
        }
    }

    private func report(_ error: Error) { lastError = String(describing: error) }
    func clearError() { lastError = nil }

    // MARK: ツリーの組み立て

    private static func buildFolderTree(root: PhotoRoot, folders: [PhotoFolder]) -> FolderNode {
        var children: [Int64: [PhotoFolder]] = [:]
        for f in folders { if let p = f.parentID { children[p, default: []].append(f) } }
        func node(_ f: PhotoFolder, name: String) -> FolderNode {
            let kids = (children[f.id] ?? []).sorted { $0.relativePath < $1.relativePath }.map { node($0, name: $0.name) }
            let total = f.photoCount + kids.reduce(0) { $0 + $1.photoCount }
            return FolderNode(id: f.id, name: name, photoCount: total, children: kids.isEmpty ? nil : kids)
        }
        guard let top = folders.first(where: { $0.parentID == nil }) else {
            return FolderNode(id: -root.id, name: root.path, photoCount: 0, children: nil)
        }
        let label = root.label.isEmpty ? (root.path.split(separator: "/").last.map(String.init) ?? root.path) : root.label
        return node(top, name: label)
    }

    /// ルートをボリュームごとにまとめる。ボリュームを判別できないルートは「その他」に入れる
    private static func buildVolumes(roots: [PhotoRoot], trees: [FolderNode], importDestination: URL) -> [VolumeGroup] {
        let destination = importDestination.standardizedFileURL.path
        var groups: [String: VolumeGroup] = [:]
        var order: [String] = []
        for (root, tree) in zip(roots, trees) {
            let key = root.volumeID.isEmpty ? "" : root.volumeID
            let entry = RootEntry(id: root.id, path: root.path, node: tree, isOnline: root.isOnline,
                                  isImportDestination: URL(fileURLWithPath: root.path).standardizedFileURL.path == destination)
            if groups[key] == nil {
                let name = root.volumeID.isEmpty ? String(localized: "Other Locations") : root.volumeName
                groups[key] = VolumeGroup(id: key, name: name, isOnline: false, roots: [])
                order.append(key)
            }
            groups[key]!.roots.append(entry)
        }
        var result = order.map { key -> VolumeGroup in
            let g = groups[key]!
            return VolumeGroup(id: g.id, name: g.name, isOnline: g.roots.contains { $0.isOnline }, roots: g.roots)
        }
        // つながっているボリュームが先、外れているものは後ろ。「その他」は最後
        result.sort { a, b in
            if a.id.isEmpty != b.id.isEmpty { return !a.id.isEmpty }
            if a.isOnline != b.isOnline { return a.isOnline }
            return a.name.localizedStandardCompare(b.name) == .orderedAscending
        }
        return result
    }

    private static func buildAlbumTree(_ albums: [Album]) -> [AlbumNode] {
        var children: [Int64: [Album]] = [:]
        var tops: [Album] = []
        for a in albums {
            if let p = a.parentID { children[p, default: []].append(a) } else { tops.append(a) }
        }
        func node(_ a: Album) -> AlbumNode {
            let kids = (children[a.id] ?? []).map(node)
            return AlbumNode(album: a, children: a.kind == .folder ? kids : nil)
        }
        return tops.map(node)
    }

    private static func buildTagTree(_ tags: [PhotoTag]) -> [TagNode] {
        var children: [Int64: [PhotoTag]] = [:]
        var tops: [PhotoTag] = []
        for t in tags {
            if let p = t.parentID { children[p, default: []].append(t) } else { tops.append(t) }
        }
        func node(_ t: PhotoTag) -> TagNode {
            let kids = (children[t.id] ?? []).map(node)
            return TagNode(id: t.id, name: t.name, photoCount: t.photoCount, children: kids.isEmpty ? nil : kids)
        }
        return tops.map(node)
    }

    // MARK: 絞り込み（9.5 章）

    /// 絞り込みの条件があるか（サイドバーで選んだ集まりは含めない）
    var isFiltering: Bool {
        filter.minRating > 0 || filter.flag != .any || filter.folderID != nil || filter.tagID != nil
            || filter.dateFrom != nil || filter.dateTo != nil
    }

    /// 絞り込みの条件の要約（ツールバーに出す）。条件がなければ nil
    var filterSummary: String? {
        var parts: [String] = []
        if filter.minRating > 0 { parts.append(String(localized: "≥ \(String(repeating: "★", count: filter.minRating))")) }
        switch filter.flag {
        case .any: break
        case .picked: parts.append(String(localized: "Picked"))
        case .rejected: parts.append(String(localized: "Rejected"))
        case .unflagged: parts.append(String(localized: "Unflagged"))
        case .notRejected: parts.append(String(localized: "Not rejected"))
        }
        if let id = filter.folderID, let name = Self.find(id, in: folderTree)?.name { parts.append(name) }
        if let id = filter.tagID, let name = Self.find(id, in: tagTree)?.name { parts.append("#" + name) }
        if filter.dateFrom != nil || filter.dateTo != nil {
            parts.append("\(filter.dateFrom ?? "")〜\(filter.dateTo ?? "")")
        }
        return parts.isEmpty ? nil : parts.joined(separator: "・")
    }

    func clearFilters() {
        var f = filter
        f.minRating = 0
        f.flag = .any
        f.folderID = nil
        f.tagID = nil
        f.dateFrom = nil
        f.dateTo = nil
        filter = f
    }

    static func find(_ id: Int64, in tree: [FolderNode]) -> FolderNode? {
        for n in tree {
            if n.id == id { return n }
            if let c = n.children, let hit = find(id, in: c) { return hit }
        }
        return nil
    }

    static func find(_ id: Int64, in tree: [TagNode]) -> TagNode? {
        for n in tree {
            if n.id == id { return n }
            if let c = n.children, let hit = find(id, in: c) { return hit }
        }
        return nil
    }

    // MARK: 何を表示するか・アルバム（9.5 章、v3.16）

    /// サイドバーで選ぶ写真の集まり。絞り込みバーの条件はこれに重ねる。
    /// album(id) は手で集めるアルバムとスマートアルバムの両方（id は同じ表）。folder(id) はストレージのフォルダ（v3.19）
    enum Source: Hashable {
        case all, recentImport, album(Int64), folder(Int64)
    }

    var source: Source {
        get {
            if let id = filter.albumID ?? filter.smartAlbumID { return .album(id) }
            if filter.recentImport { return .recentImport }
            if let id = filter.folderID { return .folder(id) }
            return .all
        }
        set {
            var f = filter
            f.albumID = nil
            f.smartAlbumID = nil
            f.recentImport = false
            f.folderID = nil
            switch newValue {
            case .all: break
            case .recentImport: f.recentImport = true
            case .album(let id):
                if albums.first(where: { $0.id == id })?.kind == .smart { f.smartAlbumID = id } else { f.albumID = id }
            case .folder(let id): f.folderID = id
            }
            filter = f
            updateWatcher()
        }
    }

    /// アルバムの名前を聞くダイアログ（ContentView が出す）
    enum AlbumPrompt: Identifiable {
        case create(addSelection: Bool, parent: Int64? = nil)
        case createFolder(parent: Int64? = nil)
        case rename(Album)
        /// スマートアルバムの作成（editing が nil）・条件の編集
        case smart(editing: Album?, parent: Int64? = nil)
        var id: String {
            switch self {
            case .create(let s, let p): "create-\(s)-\(p ?? 0)"
            case .createFolder(let p): "folder-\(p ?? 0)"
            case .rename(let a): "rename-\(a.id)"
            case .smart(let a, let p): "smart-\(a?.id ?? 0)-\(p ?? 0)"
            }
        }
    }
    var albumPrompt: AlbumPrompt?
    /// 削除の確認を出すアルバム
    var albumToDelete: Album?

    /// 新しいアルバムの名前の候補（「新規アルバム」「新規アルバム 2」…）
    func untitledAlbumName() -> String {
        let base = String(localized: "Untitled Album")
        var name = base, n = 2
        while albums.contains(where: { $0.name == name }) {
            name = "\(base) \(n)"
            n += 1
        }
        return name
    }

    func untitledFolderName() -> String {
        let base = String(localized: "Untitled Folder")
        var name = base, n = 2
        while albums.contains(where: { $0.name == name }) {
            name = "\(base) \(n)"
            n += 1
        }
        return name
    }

    func untitledSmartAlbumName() -> String {
        let base = String(localized: "Untitled Smart Album")
        var name = base, n = 2
        while albums.contains(where: { $0.name == name }) {
            name = "\(base) \(n)"
            n += 1
        }
        return name
    }

    /// 作ったアルバムを表示する。addSelection なら選択中の写真を入れる
    func createAlbum(named name: String, addSelection: Bool, parent: Int64? = nil) {
        let ids = addSelection ? targetIDs : []
        do {
            let id = try catalog.createAlbum(name, parentID: parent)
            if !ids.isEmpty { try catalog.addToAlbum(id, photoIDs: ids) }
            reloadSidebar()
            if !addSelection { source = .album(id) }
        } catch { report(error) }
    }

    func createAlbumFolder(named name: String, parent: Int64?) {
        do {
            try catalog.createAlbumFolder(name, parentID: parent)
            reloadSidebar()
        } catch { report(error) }
    }

    /// スマートアルバムを作る（editing が nil）か、条件を変える。条件が不正ならエラーを投げる（シートに出す）
    func saveSmartAlbum(editing: Album?, name: String, queryJSON: String, parent: Int64?) throws {
        if let a = editing {
            try catalog.setSmartQuery(a.id, queryJSON: queryJSON)
            if a.name != name { try catalog.renameAlbum(a.id, to: name) }
            reloadSidebar()
            if filter.smartAlbumID == a.id { reloadPhotos() }
        } else {
            let id = try catalog.createSmartAlbum(name, queryJSON: queryJSON, parentID: parent)
            reloadSidebar()
            source = .album(id)
        }
    }

    func smartQuery(of album: Album) -> SmartQuery {
        (try? catalog.smartQuery(album.id)).flatMap(SmartQuery.init(jsonString:)) ?? SmartQuery()
    }

    /// 親を変える（nil でいちばん上）
    func moveAlbum(_ album: Album, toParent parent: Int64?) {
        do {
            try catalog.moveAlbum(album.id, toParent: parent)
            reloadSidebar()
        } catch { report(error) }
    }

    /// ドラッグで動かす（フォルダの中へ、または nil でいちばん上へ）。自分の中・同じ名前がある場所など動かせないものは飛ばす。
    /// 1 つでも動かしたら true
    @discardableResult
    func moveAlbums(_ ids: [Int64], toParent parent: Int64?) -> Bool {
        var moved = false
        for id in ids {
            guard let album = albums.first(where: { $0.id == id }), album.parentID != parent else { continue }
            if let parent, !folders(forMoving: album).contains(where: { $0.id == parent }) { continue }
            do {
                try catalog.moveAlbum(id, toParent: parent)
                moved = true
            } catch { report(error) }
        }
        if moved { reloadSidebar() }
        return moved
    }

    /// album の下に移せるフォルダ（自分と自分の子孫は除く）
    func folders(forMoving album: Album) -> [Album] {
        var excluded: Set<Int64> = [album.id]
        var changed = true
        while changed {
            changed = false
            for a in albums where !excluded.contains(a.id) {
                if let p = a.parentID, excluded.contains(p) {
                    excluded.insert(a.id)
                    changed = true
                }
            }
        }
        return albums.filter { $0.kind == .folder && !excluded.contains($0.id) }
    }

    func renameAlbum(_ album: Album, to name: String) {
        do {
            try catalog.renameAlbum(album.id, to: name)
            reloadSidebar()
        } catch { report(error) }
    }

    /// アルバムを消す（写真は消えない）。フォルダなら中のアルバムも消える。表示中なら「すべての写真」に戻す
    func deleteAlbum(_ album: Album) {
        do {
            let removed = Set([album.id] + albums.filter { a in
                var p = a.parentID
                while let id = p {
                    if id == album.id { return true }
                    p = albums.first { $0.id == id }?.parentID
                }
                return false
            }.map(\.id))
            try catalog.deleteAlbum(album.id)
            if case .album(let shown) = source, removed.contains(shown) { source = .all }
            reloadSidebar()
        } catch { report(error) }
    }

    func addToAlbum(_ albumID: Int64, photoIDs ids: [Int64]? = nil) {
        let ids = ids ?? targetIDs
        guard !ids.isEmpty else { return }
        do {
            try catalog.addToAlbum(albumID, photoIDs: ids)
            reloadSidebar()
            if source == .album(albumID) { reloadPhotos() }
        } catch { report(error) }
    }

    /// 表示中の集まりが、手で集めるアルバム（写真を外せる）か
    var currentAlbumAcceptsPhotos: Bool {
        if case .album(let id) = source { return albums.first { $0.id == id }?.acceptsPhotos ?? false }
        return false
    }

    /// 表示中のアルバムから外す
    func removeFromCurrentAlbum() {
        guard case .album(let albumID) = source, currentAlbumAcceptsPhotos else { return }
        let ids = targetIDs
        guard !ids.isEmpty else { return }
        do {
            try catalog.removeFromAlbum(albumID, photoIDs: ids)
            reloadSidebar()
            reloadPhotos()
        } catch { report(error) }
    }
}
