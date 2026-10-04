import FocalCore
import SwiftUI

/// グリッドの実装（9.1 章: M2 で比較して決める）。既定は NSCollectionView。
/// UI テストや比較のため、環境変数 FOCAL_GRID=lazy で LazyVGrid に切り替えられる
enum GridImplementation: String {
    case collectionView, lazy

    static var current: GridImplementation {
        ProcessInfo.processInfo.environment["FOCAL_GRID"] == "lazy" ? .lazy : .collectionView
    }
}

struct ContentView: View {
    @Bindable var model: LibraryModel
    @State private var keys = KeyMonitor()
    @State private var frameStats = FrameStats()
    /// 現像画面の下のフィルムストリップ（9.5 章。表示メニューで隠せる）
    @AppStorage("filmstrip.visible") private var showFilmstrip = true

    var body: some View {
        NavigationSplitView {
            SidebarView(model: model)
                .navigationSplitViewColumnWidth(min: 200, ideal: 240, max: 360)
        } detail: {
            VStack(spacing: 0) {
                if model.showFilterBar {
                    FilterBar(model: model)
                    Divider()
                }
                Group {
                    switch model.mode {
                    case .grid where model.roots.isEmpty && model.importProgress == nil:
                        EmptyCatalogView(model: model)
                    case .grid:
                        switch GridImplementation.current {
                        case .collectionView: PhotoCollectionView(model: model)
                        case .lazy: PhotoLazyGrid(model: model)
                        }
                    case .viewer:
                        VStack(spacing: 0) {
                            ViewerView(model: model)
                            if showFilmstrip {
                                Divider()
                                PhotoCollectionView(model: model, style: .filmstrip)
                                    .frame(height: PhotoCollectionView.filmstripHeight)
                            }
                        }
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                Divider()
                BottomBar(model: model)
                if FrameStats.enabled {
                    // UI テストが読む計測値（FOCAL_FRAME_STATS=1 のときだけ）
                    HStack {
                        Text(frameStats.summary).accessibilityIdentifier("frameStats")
                        Text(model.develop.timingSummary).accessibilityIdentifier("developStats")
                        Button("Reset") { frameStats.reset() }.accessibilityIdentifier("frameStatsReset")
                    }
                    .font(.caption.monospaced())
                    .background(FrameStatsProbe(stats: frameStats))
                }
            }
            .inspector(isPresented: $model.showInspector) {
                // インスペクタは別の NSHostingView に入れる（スライダーを動かすたびに、ウィンドウ全体の大きさを測り直さない）
                IsolatedHost { InspectorView(model: model) }
                    .accessibilityElement(children: .contain)
                    .inspectorColumnWidth(min: 270, ideal: 300, max: 420)
            }
        }
        .overlay(alignment: .top) {
            if let p = model.importProgress {
                ProgressView(value: Double(p.done), total: Double(max(p.total, 1))) {
                    Text(p.total == 0 ? "Scanning…" : "Importing \(p.done) / \(p.total)")
                }
                .padding(10)
                .frame(maxWidth: 360)
                .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 8))
                .padding(.top, 8)
            }
        }
        .sheet(item: $model.albumPrompt) { prompt in
            if case .smart(let editing, let parent) = prompt {
                SmartAlbumSheet(model: model, editing: editing, parent: parent)
            } else {
                AlbumNameSheet(model: model, prompt: prompt)
            }
        }
        .sheet(isPresented: $model.showImport) {
            ImportSheet(model: model, importer: model.cardImport) {
                if model.cardImport.phase == .running { model.cardImport.cancel() }
                model.showImport = false
            }
        }
        .confirmationDialog(
            Text("Delete the album “\(model.albumToDelete?.name ?? "")”?"),
            isPresented: Binding(get: { model.albumToDelete != nil }, set: { if !$0 { model.albumToDelete = nil } })) {
            Button(model.albumToDelete?.kind == .folder ? "Delete Folder" : "Delete Album", role: .destructive) {
                if let a = model.albumToDelete { model.deleteAlbum(a) }
                model.albumToDelete = nil
            }
        } message: {
            if model.albumToDelete?.kind == .folder {
                Text("The albums inside are deleted too. The photos are not deleted.")
            } else {
                Text("The photos are not deleted.")
            }
        }
        .confirmationDialog(
            Text("Remove “\(model.rootToRemove?.node.name ?? "")” from the catalog?"),
            isPresented: Binding(get: { model.rootToRemove != nil }, set: { if !$0 { model.rootToRemove = nil } })) {
            Button("Remove from Catalog", role: .destructive) {
                if let r = model.rootToRemove { model.removeRoot(r) }
                model.rootToRemove = nil
            }
        } message: {
            Text("The photos’ ratings, flags, tags, album memberships and edits in this catalog are deleted. The files on disk are not deleted.")
        }
        .confirmationDialog(
            deleteTitle,
            isPresented: Binding(get: { model.deleteRequest != nil }, set: { if !$0 { model.deleteRequest = nil } }),
            presenting: model.deleteRequest) { request in
            Button(request.plan.networkPhotos > 0 ? "Delete Permanently" : "Move to Trash", role: .destructive) {
                model.confirmDelete(request)
            }
            .accessibilityIdentifier("deleteConfirm")
        } message: { request in
            Text(deleteMessage(request.plan))
        }
        .alert("Delete", isPresented: Binding(get: { model.deleteReport != nil }, set: { if !$0 { model.deleteReport = nil } })) {
            Button("OK") { model.deleteReport = nil }
        } message: {
            Text(model.deleteReport ?? "")
        }
        .alert("Error", isPresented: Binding(get: { model.lastError != nil }, set: { if !$0 { model.clearError() } })) {
            Button("OK") { model.clearError() }
        } message: {
            Text(model.lastError ?? "")
        }
        .sheet(isPresented: $model.showExport) {
            ExportSheet(export: model.export, count: model.exportIDs.count,
                        onStart: { model.startExport() },
                        onClose: {
                            if model.export.phase == .running { model.export.cancel() }
                            model.showExport = false
                        })
        }
        .onAppear { keys.install(model: model) }
        .onDisappear { keys.uninstall() }
        .onReceive(NotificationCenter.default.publisher(for: NSControl.textDidBeginEditingNotification)) { _ in
            model.develop.isEditingText = true
        }
        .onReceive(NotificationCenter.default.publisher(for: NSControl.textDidEndEditingNotification)) { _ in
            model.develop.isEditingText = false
        }
        .toolbar {
            // 一覧 / 現像の切り替え（9.5 章）
            ToolbarItem(placement: .principal) {
                Picker("Mode", selection: $model.mode) {
                    Text(LocalizedStringResource("mode.library", defaultValue: "Library")).tag(MainMode.grid)
                    Text(LocalizedStringResource("mode.develop", defaultValue: "Develop")).tag(MainMode.viewer)
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .fixedSize()
                .help("Switch between Library and Develop (V)")
                .accessibilityIdentifier("modePicker")
            }
            ToolbarItem {
                FilterToolbarButton(model: model)
            }
            ToolbarItem {
                Button { model.showInspector.toggle() } label: { Label("Inspector", systemImage: "sidebar.right") }
            }
        }
    }
}

/// 下部バー（9.5 章）。一覧ではサムネイルの大きさと枚数、現像では枚数（フィルムストリップは別に置く）
struct BottomBar: View {
    @Bindable var model: LibraryModel

    var body: some View {
        HStack(spacing: 16) {
            if model.mode == .grid {
                Slider(value: $model.thumbnailSize, in: 80...320) { Text("Size") }
                    .frame(minWidth: 60, maxWidth: 160)
            }
            Spacer(minLength: 8)
            if model.selection.count > 1 {
                Text("\(model.selection.count) selected")
                    .foregroundStyle(.secondary)
                    .accessibilityValue(model.selectedPositions.map(String.init).joined(separator: ","))
                    .accessibilityIdentifier("selectionCount")
            }
            Text(model.currentIndex.map { "\($0 + 1) / \(model.photoIDs.count)" } ?? "0 / \(model.photoIDs.count)")
                .monospacedDigit()
                .foregroundStyle(.secondary)
                .accessibilityIdentifier("photoCount")
        }
        .controlSize(.small)
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
    }
}

/// フォルダをまだ追加していないカタログ
struct EmptyCatalogView: View {
    let model: LibraryModel

    var body: some View {
        ContentUnavailableView {
            Label("No Folders", systemImage: "photo.on.rectangle.angled")
        } description: {
            Text("Add a folder that contains RAW files. The original files are never modified.")
        } actions: {
            Button("Add Folder…") { chooseFolderToAdd(model: model) }
                .buttonStyle(.borderedProminent)
                .accessibilityIdentifier("emptyAddFolder")
        }
    }
}

/// 中身を別の NSHostingView で描く。中身の更新（レイアウトのやり直し）が外側のウィンドウに伝わらないようにする。
/// 大きさは外側が決める（中身から大きさの制約を出さない: sizingOptions = []）
struct IsolatedHost<Content: View>: NSViewRepresentable {
    @ViewBuilder let content: () -> Content

    func makeNSView(context: Context) -> NSHostingView<Content> {
        let v = NSHostingView(rootView: content())
        v.sizingOptions = []
        return v
    }

    func updateNSView(_ v: NSHostingView<Content>, context: Context) {
        v.rootView = content()
    }

    func sizeThatFits(_ proposal: ProposedViewSize, nsView: NSHostingView<Content>, context: Context) -> CGSize? {
        CGSize(width: proposal.width ?? 300, height: proposal.height ?? 600)
    }
}

/// ツールバーの「絞り込み」（9.5 章）。バーを開閉し、絞り込み中は閉じていても条件の要約と解除を出す
struct FilterToolbarButton: View {
    @Bindable var model: LibraryModel

    var body: some View {
        HStack(spacing: 4) {
            if let summary = model.filterSummary, !model.showFilterBar {
                Text(summary)
                    .lineLimit(1)
                    .truncationMode(.tail)
                    .frame(maxWidth: 220)
                    .font(.callout)
                    .accessibilityIdentifier("filterSummary")
                Button { model.clearFilters() } label: { Image(systemName: "xmark.circle.fill") }
                    .buttonStyle(.borderless)
                    .help("Clear Filters")
            }
            Button {
                withAnimation(.easeInOut(duration: 0.15)) { model.showFilterBar.toggle() }
            } label: {
                Label("Filter", systemImage: model.isFiltering ? "line.3.horizontal.decrease.circle.fill"
                                                                : "line.3.horizontal.decrease.circle")
            }
            .help("Show or hide the filter bar")
            .accessibilityIdentifier("filterButton")
        }
    }
}

/// アルバムの名前を入れるシート（作る・名前の変更）
struct AlbumNameSheet: View {
    let model: LibraryModel
    let prompt: LibraryModel.AlbumPrompt
    @State private var name = ""
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(title).font(.headline)
            TextField("Name", text: $name)
                .frame(width: 280)
                .onSubmit(commit)
                .accessibilityIdentifier("albumName")
            HStack {
                Spacer()
                Button("Cancel", role: .cancel) { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button(isCreate ? "Create" : "Rename", action: commit)
                    .keyboardShortcut(.defaultAction)
                    .disabled(name.trimmingCharacters(in: .whitespaces).isEmpty)
                    .accessibilityIdentifier("albumNameOK")
            }
        }
        .padding(20)
        .onAppear {
            switch prompt {
            case .create: name = model.untitledAlbumName()
            case .createFolder: name = model.untitledFolderName()
            case .rename(let a): name = a.name
            case .smart: break
            }
        }
    }

    private var isCreate: Bool {
        switch prompt {
        case .create, .createFolder: true
        default: false
        }
    }

    private var title: LocalizedStringKey {
        switch prompt {
        case .create(let addSelection, _): addSelection ? "New Album with Selected Photos" : "New Album"
        case .createFolder: "New Folder"
        case .rename(let a): a.kind == .folder ? "Rename Folder" : "Rename Album"
        case .smart: "New Smart Album"
        }
    }

    private func commit() {
        let n = name.trimmingCharacters(in: .whitespaces)
        guard !n.isEmpty else { return }
        switch prompt {
        case .create(let addSelection, let parent): model.createAlbum(named: n, addSelection: addSelection, parent: parent)
        case .createFolder(let parent): model.createAlbumFolder(named: n, parent: parent)
        case .rename(let a): model.renameAlbum(a, to: n)
        case .smart: break
        }
        dismiss()
    }
}

extension ContentView {
    var deleteTitle: Text {
        guard let p = model.deleteRequest?.plan else { return Text("") }
        return p.networkPhotos > 0 ? Text("Delete \(p.photos) photos permanently?") : Text("Move \(p.photos) photos to the Trash?")
    }

    func deleteMessage(_ p: DeletePlan) -> String {
        var lines: [String] = []
        lines.append(String(localized: "The photos and their JPEG, video and sidecar files with the same name (\(p.files) files) will be deleted from the disk."))
        if p.networkPhotos > 0 {
            lines.append(String(localized: "\(p.networkPhotos) photos are on a network volume. They are deleted immediately and permanently, and cannot be recovered from the Trash."))
        }
        if p.networkPhotos < p.photos - p.missingPhotos {
            lines.append(String(localized: "Photos on this Mac are moved to the Trash."))
        }
        if p.missingPhotos > 0 {
            lines.append(String(localized: "\(p.missingPhotos) photos are already missing from the disk; only their catalog entries are removed."))
        }
        lines.append(String(localized: "Ratings, flags, tags, album memberships and edits in the catalog are deleted. This cannot be undone."))
        return lines.joined(separator: "\n")
    }
}
