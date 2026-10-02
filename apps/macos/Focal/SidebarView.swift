import FocalCore
import SwiftUI
import UniformTypeIdentifiers

/// 左ペイン（9.5 章）: どの写真の集まりを見るか（ライブラリとアルバム）。絞り込みは絞り込みバー（FilterBar）で行う
struct SidebarView: View {
    @Bindable var model: LibraryModel

    private var selection: Binding<LibraryModel.Source?> {
        Binding(get: { model.source }, set: { if let s = $0 { model.source = s } })
    }

    var body: some View {
        List(selection: selection) {
            Section("Library") {
                Label("All Photos", systemImage: "photo.on.rectangle").tag(LibraryModel.Source.all)
                    .accessibilityIdentifier("sidebarAll")
                Label("Recent Import", systemImage: "clock.arrow.circlepath").tag(LibraryModel.Source.recentImport)
                    .accessibilityIdentifier("sidebarRecent")
            }
            Section("Albums") {
                ForEach(model.albums) { album in
                    AlbumRow(model: model, album: album)
                        .tag(LibraryModel.Source.album(album.id))
                }
                // 見出しに置いたボタンはサイドバーではボタンとして扱われないので、一覧の最後の行に置く
                Button {
                    model.albumPrompt = .create(addSelection: false)
                } label: {
                    Label("New Album…", systemImage: "plus").foregroundStyle(.secondary)
                }
                .buttonStyle(.plain)
                .accessibilityIdentifier("newAlbum")
            }
        }
        .listStyle(.sidebar)
        .accessibilityIdentifier("sidebar")
    }
}

/// アルバムの行: 右クリックで名前の変更・削除、写真をドロップすると足す
private struct AlbumRow: View {
    let model: LibraryModel
    let album: Album
    @State private var targeted = false

    var body: some View {
        HStack {
            Label { Text(album.name).lineLimit(1).truncationMode(.middle) } icon: { Image(systemName: "rectangle.stack") }
            Spacer(minLength: 4)
            Text("\(album.photoCount)").foregroundStyle(.secondary).monospacedDigit()
        }
        .help(album.name)
        .padding(.vertical, 1)
        .background(targeted ? Color.accentColor.opacity(0.25) : .clear, in: RoundedRectangle(cornerRadius: 4))
        .accessibilityIdentifier("album-\(album.name)")
        .contextMenu {
            Button("Rename…") { model.albumPrompt = .rename(album) }
                .accessibilityIdentifier("renameAlbum")
            Button("Delete Album…") { model.albumToDelete = album }
                .accessibilityIdentifier("deleteAlbum")
        }
        .dropDestination(for: String.self) { items, _ in
            let ids = PhotoDrag.ids(from: items)
            guard !ids.isEmpty else { return false }
            model.addToAlbum(album.id, photoIDs: ids)
            return true
        } isTargeted: { targeted = $0 }
    }
}

/// グリッド・フィルムストリップから写真をドラッグするときの中身（"focal-photo:<id>" の文字列）
enum PhotoDrag {
    static let prefix = "focal-photo:"

    static func string(for id: Int64) -> String { prefix + String(id) }

    static func ids(from items: [String]) -> [Int64] {
        items.flatMap { $0.split(separator: "\n") }.compactMap { s in
            s.hasPrefix(prefix) ? Int64(s.dropFirst(prefix.count)) : nil
        }
    }
}
