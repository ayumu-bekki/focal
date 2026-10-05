import AppKit
import FocalCore
import SwiftUI

/// 「カタログ情報」（ファイル ▸ カタログ情報…、v3.23）: カタログの大きさ・中身の数・保管している情報・バックアップを出し、
/// 同じ画面から「最適化」を行う。最適化は、外したフォルダの保管情報（現像・★・フラグ・タグ・アルバム）を完全に消して、
/// カタログのファイルを詰める。頻繁に使う操作ではないので、何が消えるかを見てから押せる画面にしている
struct CatalogInfoView: View {
    let model: LibraryModel
    @Environment(\.dismiss) private var dismiss
    @State private var confirmOptimize = false

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Catalog Information").font(.headline)
            if let info = model.catalogInfo {
                details(info)
                Divider()
                optimizeSection(info)
            } else {
                Text("Could not read the information.").foregroundStyle(.secondary)
            }
            HStack {
                Spacer()
                Button("Done") { dismiss() }
                    .keyboardShortcut(.defaultAction)
                    .accessibilityIdentifier("catalogInfoDone")
            }
        }
        .padding(20)
        .frame(width: 460)
        .accessibilityIdentifier("catalogInfoSheet")
    }

    private func details(_ i: CatalogInfo) -> some View {
        Grid(alignment: .leadingFirstTextBaseline, horizontalSpacing: 12, verticalSpacing: 6) {
            row("Catalog") {
                VStack(alignment: .leading, spacing: 1) {
                    Text(model.catalog.url.deletingPathExtension().lastPathComponent)
                    Text(model.catalog.url.path).font(.caption).foregroundStyle(.secondary)
                        .textSelection(.enabled).lineLimit(2).truncationMode(.middle)
                }
            }
            row("Size") { Text(bytes(i.fileBytes)) }
            row("Folders") { Text("\(i.roots) folders, \(i.folders) subfolders") }
            row("Photos") {
                Text("\(i.photos) photos (RAW \(i.rawPhotos), other \(i.photos - i.rawPhotos))")
                    .accessibilityIdentifier("catalogInfoPhotos")
            }
            if i.missingPhotos > 0 {
                row("Missing") { Text("\(i.missingPhotos) photos").foregroundStyle(.orange) }
            }
            row("With Edits") { Text("\(i.editedPhotos) photos") }
            row("Albums / Tags") { Text("\(i.albums) / \(i.tags)") }
            row("Backups") {
                HStack(spacing: 8) {
                    Text("\(i.backupFiles) files, \(bytes(i.backupBytes))")
                    if i.backupFiles > 0 {
                        Button("Show in Finder") {
                            NSWorkspace.shared.activateFileViewerSelecting([model.catalog.url])
                        }
                        .controlSize(.small)
                    }
                }
            }
        }
        .font(.callout)
    }

    private func optimizeSection(_ i: CatalogInfo) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Optimize").font(.subheadline.weight(.semibold))
            Text("When you remove a folder from the catalog, its edits, ratings, flags, tags and album memberships are kept, and come back if you add the same photos again. Optimizing deletes this saved information for good and compacts the catalog file. A backup is made first.")
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            LabeledContent("Saved information") {
                Text(i.detachedItems > 0
                     ? String(localized: "\(i.detachedItems) photos (\(i.detachedEdits) with edits)")
                     : String(localized: "None"))
                    .accessibilityIdentifier("catalogInfoSaved")
            }
            .font(.callout)
            HStack(spacing: 10) {
                Button("Optimize…") {
                    if i.detachedItems > 0 { confirmOptimize = true } else { model.optimizeCatalog() }
                }
                .disabled(model.isOptimizing)
                .accessibilityIdentifier("optimizeButton")
                if model.isOptimizing {
                    ProgressView().controlSize(.small)
                    Text("Optimizing…").foregroundStyle(.secondary)
                }
            }
            if let message = model.optimizeMessage {
                Text(message)
                    .font(.callout)
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityIdentifier("optimizeResult")
            }
        }
        .confirmationDialog(Text("Delete the saved information?"), isPresented: $confirmOptimize) {
            Button("Delete and Optimize", role: .destructive) { model.optimizeCatalog() }
                .accessibilityIdentifier("optimizeConfirm")
        } message: {
            Text("The saved edits, ratings, flags, tags and album memberships of \(i.detachedItems) photos (\(i.detachedEdits) with edits) are deleted for good. A backup of the catalog is made first.")
        }
    }

    private func row<V: View>(_ title: LocalizedStringKey, @ViewBuilder _ value: () -> V) -> some View {
        GridRow(alignment: .firstTextBaseline) {
            Text(title).foregroundStyle(.secondary).gridColumnAlignment(.trailing)
            value().frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private func bytes(_ n: Int64) -> String { ByteCountFormatter.string(fromByteCount: n, countStyle: .file) }
}
