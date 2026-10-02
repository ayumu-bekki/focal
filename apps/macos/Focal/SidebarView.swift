import AppKit
import FocalCore
import SwiftUI
import UniformTypeIdentifiers

/// 左ペイン（9.5 章）: どの写真の集まりを見るか。上から ストレージ・ライブラリ・アルバム・取り込み（カードがつながっているときだけ）。
/// ストレージはボリュームごとにルートとフォルダを出し、外れているドライブはグレーにする（v3.19）。
/// 絞り込みは絞り込みバー（FilterBar）で行う
struct SidebarView: View {
    @Bindable var model: LibraryModel
    // 見出しの開閉（次回の起動でも保つ）
    @AppStorage("sidebar.storage.expanded") private var storageExpanded = true
    @AppStorage("sidebar.library.expanded") private var libraryExpanded = true
    @AppStorage("sidebar.albums.expanded") private var albumsExpanded = true

    private var selection: Binding<LibraryModel.Source?> {
        Binding(get: { model.source }, set: { if let s = $0 { model.source = s } })
    }

    var body: some View {
        List(selection: selection) {
            Section {
                if storageExpanded {
                ForEach(model.volumes) { volume in
                    VolumeRows(model: model, volume: volume)
                }
            }
            } header: {
                HStack {
                    InspectorSectionHeader(title: "Storage", expanded: $storageExpanded, identifier: "sidebarStorageHeader")
                    Button {
                        chooseFolderToAdd(model: model)
                    } label: {
                        Image(systemName: "plus")
                    }
                    .buttonStyle(.borderless)
                    .help("Add Folder…")
                    .accessibilityLabel("Add Folder…")
                    .accessibilityIdentifier("sidebarAddFolder")
                }
                .accessibilityElement(children: .contain)
            }
            Section {
                if libraryExpanded {
                Label("All Photos", systemImage: "photo.on.rectangle").tag(LibraryModel.Source.all)
                    .accessibilityIdentifier("sidebarAll")
                Label("Recent Import", systemImage: "clock.arrow.circlepath").tag(LibraryModel.Source.recentImport)
                    .accessibilityIdentifier("sidebarRecent")
            }
            } header: {
                InspectorSectionHeader(title: "Library", expanded: $libraryExpanded, identifier: "sidebarLibraryHeader")
            }
            Section {
                if albumsExpanded {
                ForEach(model.albumTree) { node in
                    AlbumRows(model: model, node: node)
                }
            }
            } header: {
                HStack {
                    InspectorSectionHeader(title: "Albums", expanded: $albumsExpanded, identifier: "sidebarAlbumsHeader")
                    Menu {
                        Button("New Album…") { model.albumPrompt = .create(addSelection: false) }
                            .accessibilityIdentifier("newAlbumItem")
                        Button("New Smart Album…") { model.albumPrompt = .smart(editing: nil) }
                            .accessibilityIdentifier("newSmartAlbumItem")
                        Button("New Folder…") { model.albumPrompt = .createFolder() }
                            .accessibilityIdentifier("newFolderItem")
                    } label: {
                        Image(systemName: "plus")
                    }
                    .menuStyle(.borderlessButton)
                    .menuIndicator(.hidden)
                    .fixedSize()
                    .help("New Album")
                    .accessibilityLabel("New Album")
                    .accessibilityIdentifier("newAlbum")
                }
                // アルバムを見出しにドロップすると、いちばん上へ出す
                .dropDestination(for: String.self) { items, _ in
                    model.moveAlbums(AlbumDrag.ids(from: items), toParent: nil)
                }
                .accessibilityElement(children: .contain)
            }
            if !model.importSources.isEmpty {
                Section("Import") {
                    ForEach(model.importSources) { source in
                        Button {
                            model.beginCardImport(source: source)
                        } label: {
                            Label {
                                Text("Import from “\(source.name)”…").lineLimit(1).truncationMode(.middle)
                            } icon: {
                                Image(systemName: "sdcard")
                            }
                        }
                        .buttonStyle(.plain)
                        .accessibilityIdentifier("sidebarCard-\(source.name)")
                    }
                }
            }
        }
        .listStyle(.sidebar)
        .accessibilityIdentifier("sidebar")
    }
}

// MARK: ストレージ（v3.19）

/// ボリューム → ルート → フォルダの順に入れ子で出す
private struct VolumeRows: View {
    let model: LibraryModel
    let volume: VolumeGroup
    @State private var expanded = true

    var body: some View {
        DisclosureGroup(isExpanded: $expanded) {
            ForEach(volume.roots) { root in
                FolderRows(model: model, node: root.node, root: root, online: root.isOnline)
            }
        } label: {
            Label {
                Text(volume.name).lineLimit(1).truncationMode(.middle)
            } icon: {
                Image(systemName: volume.isOnline ? "internaldrive" : "externaldrive.badge.xmark")
            }
            .foregroundStyle(volume.isOnline ? .primary : .secondary)
            .help(volume.isOnline ? volume.name : String(localized: "“\(volume.name)” is not connected."))
            .accessibilityIdentifier("volume-\(volume.name)")
        }
    }
}

private struct FolderRows: View {
    let model: LibraryModel
    let node: FolderNode
    /// ルートのときだけ
    let root: RootEntry?
    let online: Bool
    @State private var expanded: Bool

    init(model: LibraryModel, node: FolderNode, root: RootEntry? = nil, online: Bool) {
        self.model = model
        self.node = node
        self.root = root
        self.online = online
        _expanded = State(initialValue: root != nil)
    }

    var body: some View {
        if let children = node.children {
            DisclosureGroup(isExpanded: $expanded) {
                ForEach(children) { FolderRows(model: model, node: $0, online: online) }
            } label: { label }
        } else {
            label
        }
    }

    private var label: some View {
        HStack {
            Label {
                Text(node.name).lineLimit(1).truncationMode(.middle)
            } icon: {
                Image(systemName: root == nil ? "folder" : "externaldrive")
            }
            if root?.isImportDestination == true {
                Image(systemName: "star.fill").font(.caption2).foregroundStyle(.secondary)
                    .help("Import destination")
            }
            Spacer(minLength: 4)
            Text("\(node.photoCount)").foregroundStyle(.secondary).monospacedDigit()
        }
        .foregroundStyle(online ? .primary : .secondary)
        .tag(LibraryModel.Source.folder(node.id))
        .help(online ? node.name : String(localized: "Offline"))
        .accessibilityIdentifier("folder-\(node.name)")
        .contextMenu {
            if let root {
                Button("Rescan") { model.rescan(rootID: root.id) }
                    .disabled(!root.isOnline)
                Button("Set as Import Destination") {
                    model.cardImport.setDestination(URL(fileURLWithPath: root.path))
                }
                Button("Show in Finder") {
                    NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: root.path)])
                }
                .disabled(!root.isOnline)
                Divider()
                Button("Remove from Catalog…") { model.rootToRemove = root }
                    .accessibilityIdentifier("removeRoot")
            }
        }
    }
}

// MARK: アルバム

private struct AlbumRows: View {
    let model: LibraryModel
    let node: AlbumNode
    @State private var expanded = true

    var body: some View {
        if let children = node.children {
            DisclosureGroup(isExpanded: $expanded) {
                ForEach(children) { AlbumRows(model: model, node: $0) }
            } label: { AlbumRow(model: model, album: node.album) }
        } else {
            AlbumRow(model: model, album: node.album)
        }
    }
}

/// アルバムの行: 右クリックで名前の変更・移動・削除、写真をドロップすると足す（手で集めるアルバムだけ）。
/// フォルダは選べない（中のアルバムを選ぶ）
private struct AlbumRow: View {
    let model: LibraryModel
    let album: Album
    @State private var targeted = false

    var body: some View {
        let row = HStack {
            Label { Text(album.name).lineLimit(1).truncationMode(.middle) } icon: { Image(systemName: icon) }
            Spacer(minLength: 4)
            if album.kind != .folder {
                Text("\(album.photoCount)").foregroundStyle(.secondary).monospacedDigit()
            }
        }
        .help(album.name)
        .padding(.vertical, 1)
        .background(targeted ? Color.accentColor.opacity(0.25) : .clear, in: RoundedRectangle(cornerRadius: 4))
        .accessibilityIdentifier("album-\(album.name)")
        .contextMenu { menu }

        if album.kind == .folder {
            row
                .draggable(AlbumDrag.string(for: album.id))
                .dropDestination(for: String.self) { items, _ in
                    model.moveAlbums(AlbumDrag.ids(from: items), toParent: album.id)
                } isTargeted: { targeted = $0 }
        } else if album.acceptsPhotos {
            row
                .tag(LibraryModel.Source.album(album.id))
                .draggable(AlbumDrag.string(for: album.id))
                .dropDestination(for: String.self) { items, _ in
                    let ids = PhotoDrag.ids(from: items)
                    guard !ids.isEmpty else { return false }
                    model.addToAlbum(album.id, photoIDs: ids)
                    return true
                } isTargeted: { targeted = $0 }
        } else {
            row.tag(LibraryModel.Source.album(album.id))  // スマートアルバムは読み取り専用（写真のドロップを受けない）
                .draggable(AlbumDrag.string(for: album.id))
        }
    }

    private var icon: String {
        switch album.kind {
        case .album: "rectangle.stack"
        case .folder: "folder"
        case .smart: "gearshape"
        }
    }

    @ViewBuilder private var menu: some View {
        if album.kind == .folder {
            Button("New Album Here…") { model.albumPrompt = .create(addSelection: false, parent: album.id) }
            Button("New Smart Album Here…") { model.albumPrompt = .smart(editing: nil, parent: album.id) }
            Button("New Folder Here…") { model.albumPrompt = .createFolder(parent: album.id) }
            Divider()
        }
        if album.kind == .smart {
            Button("Edit Smart Album…") { model.albumPrompt = .smart(editing: album) }
                .accessibilityIdentifier("editSmartAlbum")
        }
        Button("Rename…") { model.albumPrompt = .rename(album) }
            .accessibilityIdentifier("renameAlbum")
        let targets = model.folders(forMoving: album)
        if !targets.isEmpty || album.parentID != nil {
            Menu("Move to") {
                if album.parentID != nil { Button("Top Level") { model.moveAlbum(album, toParent: nil) } }
                ForEach(targets.filter { $0.id != album.parentID }) { folder in
                    Button(folder.name) { model.moveAlbum(album, toParent: folder.id) }
                }
            }
        }
        Divider()
        Button(album.kind == .folder ? "Delete Folder…" : "Delete Album…") { model.albumToDelete = album }
            .accessibilityIdentifier("deleteAlbum")
    }
}

/// グリッド・フィルムストリップから写真をドラッグするときの中身（"focal-photo:<id>" の文字列）
/// アルバム（フォルダ・スマートアルバムも）をサイドバーでドラッグするときの中身（"focal-album:<id>"）。フォルダへ入れる
enum AlbumDrag {
    static let prefix = "focal-album:"

    static func string(for id: Int64) -> String { prefix + String(id) }

    static func ids(from items: [String]) -> [Int64] {
        items.compactMap { $0.hasPrefix(prefix) ? Int64($0.dropFirst(prefix.count)) : nil }
    }
}

enum PhotoDrag {
    static let prefix = "focal-photo:"

    static func string(for id: Int64) -> String { prefix + String(id) }

    static func ids(from items: [String]) -> [Int64] {
        items.flatMap { $0.split(separator: "\n") }.compactMap { s in
            s.hasPrefix(prefix) ? Int64(s.dropFirst(prefix.count)) : nil
        }
    }
}
