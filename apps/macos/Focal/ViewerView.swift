import AppKit
import FocalCore
import SwiftUI

/// ビューア（9.1 章）: 現像ビュー。単キーの操作は KeyMonitor が受ける
struct ViewerView: View {
    @Bindable var model: LibraryModel

    var body: some View {
        DevelopView(model: model.develop)
            .overlay(alignment: .top) {
                if model.develop.cropMode { CropToolbar(develop: model.develop) }
            }
            .overlay {
                if model.develop.stage == .failed {
                    ContentUnavailableView("Cannot open this photo", systemImage: "exclamationmark.triangle",
                                           description: Text(model.develop.errorMessage ?? ""))
                } else if model.currentPhoto == nil {
                    Text("No photo").foregroundStyle(.secondary)
                }
            }
            .accessibilityElement(children: .contain)
            .accessibilityIdentifier("viewer")
    }
}
