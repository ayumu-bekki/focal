import AppKit
import FocalCore
import SwiftUI

/// 設定の「カタログ」タブ（v3.24）: 開いているカタログの情報、取り込みの設定（カタログごと。v3.25）、バックアップの設定と操作、最適化。
/// 最適化は、外したフォルダの保管情報（現像・★・フラグ・タグ・アルバム）を完全に消して、カタログのファイルを詰める。
/// 頻繁に使う操作ではないので、何が消えるかを見てから押せるようにしている
struct CatalogSettingsView: View {
    let state: AppState
    @AppStorage(BackupSettings.intervalKey) private var interval = BackupSettings.defaultInterval
    @AppStorage(BackupSettings.askKey) private var askOnQuit = true
    @AppStorage(BackupSettings.keepKey) private var keep = BackupSettings.defaultKeep
    @AppStorage(BackupSettings.checkKey) private var checkIntegrity = true
    @AppStorage(BackupSettings.locationKey) private var location = ""
    @State private var confirmOptimize = false

    var body: some View {
        if let model = state.model {
            Form {
                infoSection(model)
                importSection(model)
                backupSection(model)
                optimizeSection(model)
            }
            .formStyle(.grouped)
            .task(id: ObjectIdentifier(model)) {
                await model.refreshCatalogInfo()
                model.backup.refresh()
            }
        } else {
            Text("No catalog is open.").foregroundStyle(.secondary).padding(40)
        }
    }

    // MARK: 情報

    @ViewBuilder private func infoSection(_ model: LibraryModel) -> some View {
        Section("Catalog Information") {
            if let i = model.catalogInfo {
                LabeledContent("Catalog") {
                    VStack(alignment: .trailing, spacing: 1) {
                        Text(model.catalog.url.deletingPathExtension().lastPathComponent)
                        Text(model.catalog.url.path).font(.caption).foregroundStyle(.secondary)
                            .textSelection(.enabled).lineLimit(2).truncationMode(.middle)
                    }
                }
                LabeledContent("Size") { Text(bytes(i.fileBytes)) }
                LabeledContent("Folders") { Text("\(i.roots) folders, \(i.folders) subfolders") }
                LabeledContent("Photos") {
                    Text("\(i.photos) photos (RAW \(i.rawPhotos), other \(i.photos - i.rawPhotos))")
                        .accessibilityIdentifier("catalogInfoPhotos")
                }
                if i.missingPhotos > 0 {
                    LabeledContent("Missing") { Text("\(i.missingPhotos) photos").foregroundStyle(.orange) }
                }
                LabeledContent("With Edits") { Text("\(i.editedPhotos) photos") }
                LabeledContent("Albums / Tags") { Text("\(i.albums) / \(i.tags)") }
                LabeledContent("Safety Backups") {
                    HStack(spacing: 8) {
                        Text("\(i.backupFiles) files, \(bytes(i.backupBytes))")
                        if i.backupFiles > 0 {
                            Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([model.catalog.url]) }
                                .controlSize(.small)
                        }
                    }
                }
            } else {
                Text("Could not read the information.").foregroundStyle(.secondary)
            }
        }
    }

    // MARK: 取り込み（このカタログの設定）

    @ViewBuilder private func importSection(_ model: LibraryModel) -> some View {
        Section {
            Toggle("Rescan folders when Focal opens", isOn: Binding(
                get: { model.prefs.rescanOnLaunch }, set: { model.prefs.rescanOnLaunch = $0 }))
                .accessibilityIdentifier("rescanOnLaunch")
            LabeledContent("Import Destination") {
                HStack {
                    Text(model.cardImport.destination.path).lineLimit(1).truncationMode(.middle).textSelection(.enabled)
                    Button("Choose…") { model.cardImport.chooseDestination() }
                }
            }
        } header: {
            Text("Import")
        } footer: {
            Text("These settings are saved in this catalog. Photos imported from a card are copied to Year / Date folders (for example 2026/2026-10-02) inside the import destination.")
                .font(.caption).foregroundStyle(.secondary)
        }
    }

    // MARK: バックアップ

    @ViewBuilder private func backupSection(_ model: LibraryModel) -> some View {
        let backup = model.backup
        Section {
            Picker("Back Up Catalog", selection: $interval) {
                ForEach(BackupSettings.intervals, id: \.self) { d in Text(BackupSettings.title(forInterval: d)).tag(d) }
            }
            .accessibilityIdentifier("backupInterval")
            Toggle("Ask before backing up when Focal quits", isOn: $askOnQuit)
                .disabled(interval < 0)
                .accessibilityIdentifier("backupAskOnQuit")
            Toggle("Check the catalog before backing up", isOn: $checkIntegrity)
                .accessibilityIdentifier("backupCheckIntegrity")
            Picker("Keep", selection: $keep) {
                ForEach(BackupSettings.keeps + (BackupSettings.keeps.contains(keep) ? [] : [keep]), id: \.self) { n in
                    Text(n == 0 ? String(localized: "All backups") : String(localized: "The latest \(n) backups")).tag(n)
                }
            }
            .accessibilityIdentifier("backupKeep")
            LabeledContent("Location") {
                HStack {
                    Text(BackupSettings.location.path).lineLimit(1).truncationMode(.middle).textSelection(.enabled)
                    Button("Choose…") { chooseLocation(backup) }
                }
            }
            LabeledContent("Last Backup") {
                Text(backup.lastBackup.map { $0.formatted(date: .abbreviated, time: .shortened) }
                     ?? String(localized: "Never"))
                    .accessibilityIdentifier("backupLast")
            }
            HStack(spacing: 10) {
                Button("Back Up Now") { backup.backUpNow() }
                    .disabled(backup.isRunning)
                    .accessibilityIdentifier("backupNow")
                if backup.isRunning {
                    ProgressView(value: backup.progress).frame(width: 140)
                    Button("Cancel") { backup.cancel() }.controlSize(.small)
                }
                Spacer()
                if case .finished(let message) = backup.phase {
                    Text(message).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                        .accessibilityIdentifier("backupResult")
                } else if case .failed(let message) = backup.phase {
                    Text(message).font(.caption).foregroundStyle(.red).lineLimit(2)
                }
            }
            if !backup.items.isEmpty {
                DisclosureGroup("Recent Backups") {
                    ForEach(backup.items.prefix(8)) { item in
                        HStack {
                            Text(item.created).monospacedDigit()
                            Text(bytes(item.bytes)).foregroundStyle(.secondary)
                            Spacer()
                            Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([item.url]) }
                                .controlSize(.small)
                        }
                        .font(.callout)
                    }
                }
            }
        } header: {
            Text("Backup")
        } footer: {
            Text("A backup is a copy of the catalog that you can open as it is (File ▸ Open Catalog…). Photos and caches are not included. Backups are made when Focal quits.")
                .font(.caption).foregroundStyle(.secondary)
        }
        .alert("The catalog may be damaged", isPresented: Binding(get: { backup.damagedMessage != nil },
                                                                   set: { if !$0 { backup.damagedMessage = nil } })) {
            Button("Back Up Anyway") { backup.backUpNow(allowDamaged: true) }
            Button("Cancel", role: .cancel) { backup.damagedMessage = nil }
        } message: {
            Text("The integrity check found a problem with the catalog. A backup of a damaged catalog counts toward the number of backups kept, so older, good backups may be deleted.\n\(backup.damagedMessage ?? "")")
        }
    }

    private func chooseLocation(_ backup: BackupModel) {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false
        panel.prompt = String(localized: "Choose")
        panel.message = String(localized: "Choose the folder for catalog backups. A different disk is safer.")
        panel.directoryURL = BackupSettings.location
        if panel.runModal() == .OK, let url = panel.url {
            location = url.path
            backup.refresh()
        }
    }

    // MARK: 最適化

    @ViewBuilder private func optimizeSection(_ model: LibraryModel) -> some View {
        let saved = model.catalogInfo
        Section {
            LabeledContent("Saved information") {
                Text((saved?.detachedItems ?? 0) > 0
                     ? String(localized: "\(saved!.detachedItems) photos (\(saved!.detachedEdits) with edits)")
                     : String(localized: "None"))
                    .accessibilityIdentifier("catalogInfoSaved")
            }
            HStack(spacing: 10) {
                Button("Optimize…") {
                    if (saved?.detachedItems ?? 0) > 0 { confirmOptimize = true } else { model.optimizeCatalog() }
                }
                .disabled(model.isOptimizing)
                .accessibilityIdentifier("optimizeButton")
                if model.isOptimizing {
                    ProgressView().controlSize(.small)
                    Text("Optimizing…").foregroundStyle(.secondary)
                }
            }
            if let message = model.optimizeMessage {
                Text(message).font(.callout).fixedSize(horizontal: false, vertical: true)
                    .accessibilityIdentifier("optimizeResult")
            }
        } header: {
            Text("Optimize")
        } footer: {
            Text("When you remove a folder from the catalog, its edits, ratings, flags, tags and album memberships are kept, and come back if you add the same photos again. Optimizing deletes this saved information for good and compacts the catalog file. A backup is made first.")
                .font(.caption).foregroundStyle(.secondary)
        }
        .confirmationDialog(Text("Delete the saved information?"), isPresented: $confirmOptimize) {
            Button("Delete and Optimize", role: .destructive) { model.optimizeCatalog() }
                .accessibilityIdentifier("optimizeConfirm")
        } message: {
            Text("The saved edits, ratings, flags, tags and album memberships of \(saved?.detachedItems ?? 0) photos (\(saved?.detachedEdits ?? 0) with edits) are deleted for good. A backup of the catalog is made first.")
        }
    }

    private func bytes(_ n: Int64) -> String { ByteCountFormatter.string(fromByteCount: n, countStyle: .file) }
}
