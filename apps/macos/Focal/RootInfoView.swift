import AppKit
import FocalCore
import SwiftUI

/// 追加したフォルダの情報（サイドバーの右クリック ▸「情報を見る…」、v3.19）。場所・ディスク・容量・カタログの内容と、
/// 読み込み先・表示名・再スキャン・カタログから外す。ディスクの容量は応答しない共有で待たされることがあるので、
/// 別のスレッドで取り、取れるまでは「読み込み中…」を出す
struct RootInfoView: View {
    let model: LibraryModel
    let root: RootEntry
    let dismiss: () -> Void
    @State private var details: RootDetails?
    @State private var loaded = false
    @State private var label = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 8) {
                Image(systemName: root.isOnline ? "folder.fill" : "externaldrive.badge.xmark")
                    .font(.title2)
                    .foregroundStyle(root.isOnline ? Color.accentColor : .secondary)
                Text(root.node.name).font(.headline).lineLimit(1).truncationMode(.middle)
                Spacer(minLength: 0)
            }
            if let d = details {
                content(d)
            } else if loaded {
                Text("Could not read the information.").foregroundStyle(.secondary)
            } else {
                HStack(spacing: 6) {
                    ProgressView().controlSize(.small)
                    Text("Reading…").foregroundStyle(.secondary)
                }
            }
            Divider()
            actions
        }
        .padding(16)
        .frame(width: 380)
        .task(id: root.id) { await load() }
        .accessibilityIdentifier("rootInfoPopover")
    }

    @ViewBuilder private func content(_ d: RootDetails) -> some View {
        Grid(alignment: .leadingFirstTextBaseline, horizontalSpacing: 12, verticalSpacing: 6) {
            row("Location") {
                Text(d.path).textSelection(.enabled).lineLimit(3).truncationMode(.middle)
                    .accessibilityIdentifier("rootInfoPath")
            }
            row("Status") {
                HStack(spacing: 5) {
                    Circle().fill(d.isOnline ? Color.green : Color.secondary).frame(width: 8, height: 8)
                    Text(d.isOnline ? "Connected" : "Offline")
                }
            }
            if !d.volumeName.isEmpty || !d.mountPoint.isEmpty {
                row("Disk") {
                    VStack(alignment: .leading, spacing: 1) {
                        Text([d.volumeName, kindText(d.kind), d.fileSystem].filter { !$0.isEmpty }.joined(separator: " · "))
                        if !d.mountPoint.isEmpty, d.mountPoint != "/" {
                            Text(d.mountPoint).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                        }
                    }
                }
            }
            if let total = d.totalBytes, let free = d.freeBytes {
                row("Capacity") {
                    VStack(alignment: .leading, spacing: 3) {
                        ProgressView(value: Double(total - free), total: Double(max(total, 1)))
                        Text("\(bytes(free)) free of \(bytes(total))").font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
            row("Photos") {
                Text("\(d.photos) photos, \(d.folders) subfolders")
                    .accessibilityIdentifier("rootInfoPhotos")
            }
            if d.missing > 0 {
                row("Missing") { Text("\(d.missing) photos").foregroundStyle(.orange) }
            }
            if d.photos > 0 {
                row("Total Size") { Text(bytes(d.totalFileBytes)) }
            }
            if let from = d.captureFrom, let to = d.captureTo {
                row("Capture Dates") { Text(from == to ? from : "\(from) – \(to)") }
            }
        }
        .font(.callout)
    }

    private var actions: some View {
        VStack(alignment: .leading, spacing: 10) {
            LabeledContent("Display Name") {
                TextField("", text: $label, prompt: Text(root.node.name))
                    .textFieldStyle(.roundedBorder)
                    .onSubmit(saveLabel)
                    .accessibilityIdentifier("rootInfoName")
            }
            Toggle("Set as Import Destination", isOn: Binding(
                get: { model.isImportDestination(root) },
                set: { if $0 { model.cardImport.setDestination(URL(fileURLWithPath: root.path)) } }))
                .accessibilityIdentifier("rootInfoDestination")
            HStack {
                Button("Show in Finder") {
                    NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: root.path)])
                }
                .disabled(!root.isOnline)
                Button("Rescan") {
                    model.rescan(rootID: root.id)
                    dismiss()
                }
                .disabled(!root.isOnline)
                Button("Change Location…") {
                    dismiss()
                    model.chooseNewLocation(for: root)
                }
                .accessibilityIdentifier("rootInfoRelocate")
                Spacer()
            }
            HStack {
                Spacer()
                Button("Remove from Catalog…", role: .destructive) {
                    dismiss()
                    model.requestRemoveRoot(root)
                }
            }
        }
        .font(.callout)
    }

    private func row<V: View>(_ title: LocalizedStringKey, @ViewBuilder _ value: () -> V) -> some View {
        GridRow(alignment: .firstTextBaseline) {
            Text(title).foregroundStyle(.secondary).gridColumnAlignment(.trailing)
            value().frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private func kindText(_ kind: RootDetails.Kind) -> String {
        switch kind {
        case .unknown: ""
        case .internal: String(localized: "Internal")
        case .external: String(localized: "External")
        case .network: String(localized: "Network")
        }
    }

    private func bytes(_ n: Int64) -> String { ByteCountFormatter.string(fromByteCount: n, countStyle: .file) }

    private func load() async {
        let catalog = model.catalog
        let id = root.id
        let d = await Task.detached { try? catalog.rootDetails(rootID: id) }.value
        details = d
        loaded = true
        if let d, label.isEmpty { label = d.label }
    }

    private func saveLabel() {
        let trimmed = label.trimmingCharacters(in: .whitespaces)
        try? model.catalog.setRootLabel(trimmed, rootID: root.id)
        model.reloadSidebar()
    }
}
