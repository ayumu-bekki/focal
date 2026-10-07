import AppKit
import FocalCore
import SwiftUI

/// SD カードなどからの取り込み（⌘⇧I、v3.19）。カードのファイルを <読み込み先>/YYYY/YYYY-MM-DD/ にコピーして登録する。
/// カードには書き込まない（移動・削除はしない）
struct ImportSheet: View {
    let model: LibraryModel
    @Bindable var importer: CardImportModel
    let onClose: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Import from Card").font(.headline)
            switch importer.phase {
            case .settings: settings
            case .running: running
            case .finished: finished
            }
        }
        .padding(20)
        // 設定の画面は、写真の一覧を出すので広く、広げられる。実行中・完了は小さい
        .frame(minWidth: wide ? 780 : 500, idealWidth: wide ? 840 : 500, maxWidth: wide ? .infinity : 500,
               minHeight: wide ? 660 : nil, idealHeight: wide ? 740 : nil, maxHeight: wide ? .infinity : nil)
    }

    private var wide: Bool { importer.phase == .settings }

    // MARK: 設定

    private var settings: some View {
        VStack(alignment: .leading, spacing: 12) {
            Form {
                LabeledContent("Source") {
                    HStack {
                        sourcePicker
                        Button {
                            model.refreshImportSources()
                        } label: {
                            Image(systemName: "arrow.clockwise")
                        }
                        .buttonStyle(.borderless)
                        .help("Look for cards again")
                        Button {
                            importer.ejectSelected()
                        } label: {
                            Image(systemName: "eject")
                        }
                        .buttonStyle(.borderless)
                        .help("Eject the card")
                        .disabled(!importer.canEject)
                        .accessibilityIdentifier("importEject")
                        Button("Choose Folder…") { importer.chooseFolder() }
                            .accessibilityIdentifier("importChooseFolder")
                    }
                }
                LabeledContent("") {
                    Group {
                        if importer.isSummarizing {
                            Text("Reading the card…")
                        } else if let s = importer.summary {
                            if importer.importedCount > 0 {
                                Text("\(s.shots) photos, \(ByteCountFormatter.string(fromByteCount: s.bytes, countStyle: .file)) (\(importer.importedCount) already imported)")
                            } else {
                                Text("\(s.shots) photos, \(ByteCountFormatter.string(fromByteCount: s.bytes, countStyle: .file))")
                            }
                        } else if importer.sourceURL == nil {
                            Text("No card with a DCIM folder was found.")
                        }
                    }
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .accessibilityIdentifier("importSummary")
                }
                LabeledContent("Import Destination") {
                    HStack {
                        Text(importer.destination.path)
                            .lineLimit(1)
                            .truncationMode(.middle)
                            .help(importer.destination.path)
                        Button("Choose…") { importer.chooseDestination() }
                            .accessibilityIdentifier("importChooseDestination")
                    }
                }
                if let free = importer.freeSpace, let needed = importer.neededBytes, needed > free {
                    LabeledContent("") {
                        Text("The destination has \(ByteCountFormatter.string(fromByteCount: free, countStyle: .file)) free; the selected photos take \(ByteCountFormatter.string(fromByteCount: needed, countStyle: .file)). Photos already imported are skipped, but there may not be enough space.")
                            .font(.callout)
                            .foregroundStyle(.orange)
                            .accessibilityIdentifier("importSpaceWarning")
                    }
                }
                LabeledContent("") {
                    Text("Photos are copied to Year / Date folders, for example 2026/2026-10-02.")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                }
                Picker("Add to Album", selection: $importer.albumID) {
                    Text("None").tag(Int64?.none)
                    ForEach(model.albums.filter(\.acceptsPhotos)) { Text($0.name).tag(Int64?.some($0.id)) }
                }
                .accessibilityIdentifier("importAlbum")
                Picker("Develop Preset", selection: $importer.presetID) {
                    Text("None").tag(String?.none)
                    if !model.presets.builtIn.isEmpty {
                        Section("Focal") {
                            ForEach(model.presets.builtIn) { Text($0.name).tag(String?.some($0.id)) }
                        }
                    }
                    if !model.presets.user.isEmpty {
                        Section("My Presets") {
                            ForEach(model.presets.user) { Text($0.name).tag(String?.some($0.id)) }
                        }
                    }
                }
                .accessibilityIdentifier("importPreset")
                TextField("Tags", text: $importer.tagsText, prompt: Text("Trip/Hokkaido, 2026"))
                    .accessibilityIdentifier("importTags")
                Toggle("Verify copies", isOn: $importer.verify)
                    .accessibilityIdentifier("importVerify")
            }
            .formStyle(.columns)
            CardShotGrid(importer: importer)
                .frame(minHeight: 220)
            if let failure = importer.failure {
                Text(failure).font(.caption).foregroundStyle(.red)
            }
            HStack {
                Text("Photos on the card are never moved or deleted.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Spacer()
                Button("Cancel", action: onClose).keyboardShortcut(.cancelAction)
                Button("Import") { importer.start() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(!importer.canStart)
                    .accessibilityIdentifier("importStart")
            }
        }
    }

    @ViewBuilder private var sourcePicker: some View {
        if importer.sources.isEmpty && importer.sourceURL == nil {
            Text("No card").foregroundStyle(.secondary)
        } else {
            Picker("", selection: Binding(get: { importer.selectedSourceID ?? "" },
                                          set: { importer.selectedSourceID = $0.isEmpty ? nil : $0 })) {
                ForEach(importer.sources) { Text($0.name).tag($0.id) }
                if let folder = importer.sourceURL, importer.selectedSourceID == nil {
                    Text(folder.lastPathComponent).tag("")
                }
            }
            .labelsHidden()
            .accessibilityIdentifier("importSource")
        }
    }

    // MARK: 実行中

    private var running: some View {
        VStack(alignment: .leading, spacing: 12) {
            switch importer.progressPhase {
            case .reading:
                ProgressView(value: Double(importer.done), total: Double(max(importer.total, 1))) {
                    Text("Reading the card… \(importer.done) / \(importer.total)")
                }
            case .copying:
                ProgressView(value: Double(importer.bytesDone), total: Double(max(importer.bytesTotal, 1))) {
                    Text("Copying \(importer.done) / \(importer.total)")
                }
                Text(importer.current).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
            case .cataloging:
                ProgressView(value: Double(importer.done), total: Double(max(importer.total, 1))) {
                    Text("Adding to the catalog… \(importer.done) / \(importer.total)")
                }
            }
            HStack {
                Spacer()
                Button("Stop") { importer.cancel() }
                    .keyboardShortcut(.cancelAction)
                    .accessibilityIdentifier("importStop")
            }
        }
    }

    // MARK: 完了

    private var finished: some View {
        VStack(alignment: .leading, spacing: 10) {
            if let r = importer.result {
                Text(r.cancelled ? "Stopped. Imported \(r.imported) of \(r.shots) photos."
                                 : "Imported \(r.imported) of \(r.shots) photos.")
                    .accessibilityIdentifier("importResult")
                if r.skippedDuplicates > 0 {
                    Text("\(r.skippedDuplicates) photos were already imported and skipped.")
                        .foregroundStyle(.secondary)
                }
                if r.skippedUnselected > 0 {
                    Text("\(r.skippedUnselected) photos were not selected and skipped.")
                        .foregroundStyle(.secondary)
                }
                if r.estimatedDates > 0 {
                    Text("\(r.estimatedDates) photos have no capture time, so the file’s modified date was used (estimated).")
                        .foregroundStyle(.secondary)
                }
                if r.failed > 0 || !r.errors.isEmpty {
                    if r.failed > 0 { Text("\(r.failed) photos could not be imported.").foregroundStyle(.red) }
                    if r.filesCopied > 0 && r.added == 0 && r.imported > 0 {
                        Text("The files were copied, but could not be added to the catalog. Rescan the destination folder to add them.")
                            .foregroundStyle(.red)
                    }
                    ScrollView {
                        Text(r.errors).font(.caption).foregroundStyle(.red).frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .frame(maxHeight: 100)
                }
            }
            if let failure = importer.failure {
                Text(failure).font(.caption).foregroundStyle(.red)
            }
            HStack {
                Button("Show Imported Photos") {
                    importer.showImported()
                    onClose()
                }
                .disabled((importer.result?.added ?? 0) == 0)
                .accessibilityIdentifier("importShow")
                if importer.canEject {
                    Button("Eject Card") { importer.ejectSelected() }
                        .accessibilityIdentifier("importEjectAfter")
                }
                Spacer()
                Button("Close", action: onClose)
                    .keyboardShortcut(.defaultAction)
                    .accessibilityIdentifier("importClose")
            }
        }
    }
}
