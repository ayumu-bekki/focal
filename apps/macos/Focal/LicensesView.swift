import SwiftUI

/// 同梱しているオープンソースソフトウェアのライセンス（ADR-01）。アプリの Resources/Licenses を表示する
struct LicensesView: View {
    @State private var selection: URL?

    private var files: [URL] {
        guard let dir = Bundle.main.url(forResource: "Licenses", withExtension: nil) else { return [] }
        let urls = (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? []
        // README を先頭に、残りは名前順
        return urls.filter { $0.pathExtension == "txt" }.sorted {
            ($0.lastPathComponent == "README.txt" ? "" : $0.lastPathComponent)
                < ($1.lastPathComponent == "README.txt" ? "" : $1.lastPathComponent)
        }
    }

    var body: some View {
        NavigationSplitView {
            List(files, id: \.self, selection: $selection) { url in
                Text(url.deletingPathExtension().lastPathComponent)
            }
            .navigationSplitViewColumnWidth(min: 180, ideal: 200)
        } detail: {
            ScrollView {
                Text(selection.flatMap { try? String(contentsOf: $0, encoding: .utf8) } ?? "")
                    .font(.system(.body, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding()
            }
        }
        .frame(minWidth: 700, minHeight: 480)
        .onAppear { selection = selection ?? files.first }
    }
}
