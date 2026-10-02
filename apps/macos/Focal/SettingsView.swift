import AppKit
import FocalCore
import SwiftUI

/// 設定ウィンドウ（⌘,）: 外観、カタログの場所と、キャッシュの上限・使用量・削除
struct SettingsView: View {
    let state: AppState
    @AppStorage(AppAppearance.key) private var appearance = AppAppearance.system
    @State private var usage: UInt64?
    @State private var clearing = false

    private static let limits: [UInt64] = [512 << 20, 1 << 30, 2 << 30, 5 << 30, 10 << 30, 20 << 30]

    var body: some View {
        Form {
            Section("General") {
                Picker("Appearance", selection: $appearance) {
                    ForEach(AppAppearance.allCases) { a in Text(a.title).tag(a) }
                }
                .accessibilityIdentifier("appearance")
                .onChange(of: appearance) { _, a in a.apply() }
                // 表示を描く GPU（v3.14）。書き出しは常に CPU
                LabeledContent("Display Rendering") {
                    Text(state.model.map { m in
                        m.develop.editor.gpuName.isEmpty ? String(localized: "CPU") : "GPU (\(m.develop.editor.gpuName))"
                    } ?? "—")
                    .accessibilityIdentifier("renderingBackend")
                }
            }
            Section("Catalog") {
                LabeledContent("Location") {
                    pathRow(state.catalogURL)
                }
            }
            Section {
                Picker("Maximum Size", selection: Binding(get: { state.previewCacheLimit },
                                                          set: { state.previewCacheLimit = $0; refreshUsage() })) {
                    ForEach(Self.limits + (Self.limits.contains(state.previewCacheLimit) ? [] : [state.previewCacheLimit]),
                            id: \.self) { n in
                        Text(bytes(n)).tag(n)
                    }
                }
                .accessibilityIdentifier("previewCacheLimit")
                LabeledContent("Current Usage") {
                    HStack {
                        Text(usage.map(bytes) ?? "—")
                            .monospacedDigit()
                            .accessibilityIdentifier("previewCacheUsage")
                        Button("Clear Cache") { clear() }
                            .disabled(state.model == nil || clearing || usage == 0)
                            .accessibilityIdentifier("previewCacheClear")
                    }
                }
                LabeledContent("Location") { pathRow(AppPaths.previewCache) }
            } header: {
                Text("Preview Cache")
            } footer: {
                Text("Photos shown in the viewer are kept here so they appear instantly next time. When the limit is exceeded, the least recently used previews are removed.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Section("Thumbnail Cache") {
                LabeledContent("Location") { pathRow(AppPaths.thumbnailCache) }
            }
        }
        .formStyle(.grouped)
        .frame(width: 560)
        .fixedSize(horizontal: false, vertical: true)
        .onAppear { refreshUsage() }
    }

    private func pathRow(_ url: URL?) -> some View {
        HStack {
            Text(url.map(displayPath) ?? "—")
                .lineLimit(1)
                .truncationMode(.middle)
                .textSelection(.enabled)
            if let url {
                Button {
                    NSWorkspace.shared.activateFileViewerSelecting([url])
                } label: {
                    Image(systemName: "arrow.right.circle")
                }
                .buttonStyle(.borderless)
                .help("Show in Finder")
            }
        }
    }

    private func bytes(_ n: UInt64) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(n), countStyle: .file)
    }

    /// 使用量はフォルダを数えるので、メインスレッドの外で
    private func refreshUsage() {
        guard let editor = state.model?.develop.editor else {
            usage = nil
            return
        }
        Task {
            let n = await Task.detached { try? editor.previewCacheUsage() }.value
            usage = n
        }
    }

    private func clear() {
        guard let editor = state.model?.develop.editor else { return }
        clearing = true
        Task {
            await Task.detached { try? editor.clearPreviewCache() }.value
            clearing = false
            refreshUsage()
        }
    }
}
