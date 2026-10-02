import AppKit
import SwiftUI

/// 初回起動（カタログがまだない）の画面。新しいカタログを作るか、既存のカタログを開く
struct WelcomeView: View {
    let state: AppState

    var body: some View {
        VStack(spacing: 20) {
            Image(nsImage: NSApp.applicationIconImage)
                .resizable()
                .frame(width: 96, height: 96)
            Text("Welcome to Focal")
                .font(.largeTitle)
            Text("A catalog records your folders, ratings, tags and edits. Original files are never modified.")
                .multilineTextAlignment(.center)
                .foregroundStyle(.secondary)
                .frame(maxWidth: 420)

            VStack(spacing: 10) {
                Button {
                    state.create(AppPaths.defaultNewCatalog)
                } label: {
                    Text("Create Catalog").frame(minWidth: 220)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
                .keyboardShortcut(.defaultAction)
                .accessibilityIdentifier("welcomeCreate")
                Text(displayPath(AppPaths.defaultCatalogDirectory))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .accessibilityIdentifier("welcomeLocation")
                HStack(spacing: 12) {
                    Button("Choose Location…") { state.chooseNewCatalog() }
                    Button("Open Existing Catalog…") { state.chooseExistingCatalog() }
                        .accessibilityIdentifier("welcomeOpen")
                }
                .padding(.top, 6)
            }
            .padding(.top, 8)

            if let error = state.error {
                Text(error)
                    .foregroundStyle(.red)
                    .multilineTextAlignment(.center)
                    .frame(maxWidth: 480)
            }
        }
        .padding(40)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

/// ホームフォルダを「~」で表したパス
func displayPath(_ url: URL) -> String {
    let home = FileManager.default.homeDirectoryForCurrentUser.path
    let p = url.path
    return p.hasPrefix(home + "/") ? "~" + p.dropFirst(home.count) : p
}
