import AppKit
import FocalCore
import SwiftUI

/// 書き出し（5.8 章）の状態
@MainActor
@Observable
final class ExportModel {
    enum Phase: Equatable { case settings, running, finished }

    var format: Catalog.ExportOptions.Format = .jpeg
    var quality = 92.0
    var resize = false
    var longEdge = 2048
    var destination: URL

    private(set) var phase: Phase = .settings
    private(set) var done = 0
    private(set) var total = 0
    private(set) var succeeded = 0
    private(set) var failures: [String] = []
    private(set) var cancelled = false
    private(set) var lastOutput: URL?

    private var task: Task<Void, Never>?
    private static let destinationKey = "exportDestination"

    init() {
        let env = ProcessInfo.processInfo.environment["FOCAL_EXPORT_DIR"].map { URL(fileURLWithPath: $0) }
        let saved = UserDefaults.standard.string(forKey: Self.destinationKey).map { URL(fileURLWithPath: $0) }
        destination = env ?? saved
            ?? FileManager.default.urls(for: .picturesDirectory, in: .userDomainMask)[0]
    }

    func chooseDestination() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.directoryURL = destination
        panel.prompt = String(localized: "Choose")
        if panel.runModal() == .OK, let url = panel.url {
            destination = url
            UserDefaults.standard.set(url.path, forKey: Self.destinationKey)
        }
    }

    func start(catalog: Catalog, ids: [Int64]) {
        var opt = Catalog.ExportOptions(destination: destination)
        opt.format = format
        opt.quality = Int(quality)
        opt.longEdge = resize ? max(16, longEdge) : 0
        phase = .running
        done = 0
        total = ids.count
        succeeded = 0
        failures = []
        cancelled = false
        lastOutput = nil
        task = Task { @MainActor in
            do {
                for try await ev in catalog.export(ids, options: opt) {
                    switch ev {
                    case .item(let d, let n, _, let output, let error):
                        done = d
                        total = n
                        if let output {
                            succeeded += 1
                            lastOutput = output
                        } else if let error {
                            failures.append(error)
                        }
                    case .finished(let c):
                        cancelled = c
                    }
                }
            } catch {
                failures.append(String(describing: error))
            }
            phase = .finished
        }
    }

    func cancel() { task?.cancel() }

    func reset() { phase = .settings }
}

/// 書き出しのシート（⌘⇧E）
struct ExportSheet: View {
    @Bindable var export: ExportModel
    let count: Int
    let onStart: () -> Void
    let onClose: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Export \(count) Photos").font(.headline)
            switch export.phase {
            case .settings: settings
            case .running: running
            case .finished: finished
            }
        }
        .padding(20)
        .frame(width: 440)
    }

    private var settings: some View {
        VStack(alignment: .leading, spacing: 12) {
            Form {
                Picker("Format", selection: $export.format) {
                    Text("JPEG (sRGB)").tag(Catalog.ExportOptions.Format.jpeg)
                    Text("TIFF 16-bit (sRGB)").tag(Catalog.ExportOptions.Format.tiff16)
                }
                .accessibilityIdentifier("exportFormat")
                if export.format == .jpeg {
                    LabeledContent("Quality") {
                        HStack {
                            Slider(value: $export.quality, in: 50...100, step: 1)
                            Text("\(Int(export.quality))").monospacedDigit().frame(width: 32)
                        }
                    }
                }
                Toggle("Resize", isOn: $export.resize).accessibilityIdentifier("exportResize")
                if export.resize {
                    LabeledContent("Long edge") {
                        HStack {
                            TextField("", value: $export.longEdge, format: .number)
                                .frame(width: 80)
                                .accessibilityIdentifier("exportLongEdge")
                            Text("px")
                        }
                    }
                }
                LabeledContent("Destination") {
                    HStack {
                        Text(export.destination.path).lineLimit(1).truncationMode(.middle).help(export.destination.path)
                        Button("Choose…") { export.chooseDestination() }
                    }
                }
            }
            .formStyle(.columns)
            HStack {
                Spacer()
                Button("Cancel", action: onClose).keyboardShortcut(.cancelAction)
                Button("Export", action: onStart)
                    .keyboardShortcut(.defaultAction)
                    .disabled(count == 0)
                    .accessibilityIdentifier("exportStart")
            }
        }
    }

    private var running: some View {
        VStack(alignment: .leading, spacing: 12) {
            ProgressView(value: Double(export.done), total: Double(max(export.total, 1))) {
                Text("Exporting \(export.done) / \(export.total)")
            }
            HStack {
                Spacer()
                Button("Stop") { export.cancel() }.keyboardShortcut(.cancelAction)
            }
        }
    }

    private var finished: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(export.cancelled ? "Stopped. Exported \(export.succeeded) of \(export.total) photos."
                                  : "Exported \(export.succeeded) of \(export.total) photos.")
                .accessibilityIdentifier("exportSummary")
            if !export.failures.isEmpty {
                ScrollView {
                    VStack(alignment: .leading) {
                        ForEach(export.failures, id: \.self) { Text($0).font(.caption).foregroundStyle(.red) }
                    }
                }
                .frame(maxHeight: 100)
            }
            HStack {
                Button("Show in Finder") {
                    if let out = export.lastOutput {
                        NSWorkspace.shared.activateFileViewerSelecting([out])
                    } else {
                        NSWorkspace.shared.open(export.destination)
                    }
                }
                Spacer()
                Button("Close", action: onClose)
                    .keyboardShortcut(.defaultAction)
                    .accessibilityIdentifier("exportClose")
            }
        }
    }
}
