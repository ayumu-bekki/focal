import FocalCore
import SwiftUI

/// クロップモードのツールバー（9.1 章: 縦横比、傾き補正、水平線ツール、90° 回転）
struct CropToolbar: View {
    @Bindable var develop: DevelopModel

    private static let aspects: [(LocalizedStringKey, Int32)] = [
        ("Free", 0), ("Original", 1), ("1:1", 2), ("3:2", 3), ("4:3", 4), ("16:9", 5), ("5:4", 6),
    ]

    var body: some View {
        HStack(spacing: 14) {
            Picker("Aspect", selection: Binding(get: { develop.settings.aspect }, set: { develop.setAspect($0) })) {
                ForEach(Self.aspects, id: \.1) { Text($0.0).tag($0.1) }
            }
            .fixedSize()
            .accessibilityIdentifier("aspectPicker")

            HStack(spacing: 6) {
                Text("Straighten")
                DevSlider(value: Binding(get: { develop.settings.straighten }, set: { develop.setStraighten($0) }),
                          range: -45...45, defaultValue: 0, identifier: "straightenSlider")
                    .baselineAtCenter()
                    .frame(minWidth: 120, maxWidth: 200)
                Text(String(format: "%+.1f°", develop.settings.straighten)).monospacedDigit().frame(width: 52)
            }

            Toggle(isOn: $develop.levelTool) { Label("Level", systemImage: "level") }
                .toggleStyle(.button)
                .help(Text("Drag along a line that should be horizontal or vertical"))
                .accessibilityIdentifier("levelTool")

            Button { develop.rotate(by: -1) } label: { Label("Rotate Left", systemImage: "rotate.left") }
                .labelStyle(.iconOnly)
                .help(Text("Rotate Left"))
                .accessibilityIdentifier("rotateLeft")
            Button { develop.rotate(by: 1) } label: { Label("Rotate Right", systemImage: "rotate.right") }
                .labelStyle(.iconOnly)
                .help(Text("Rotate Right"))
                .accessibilityIdentifier("rotateRight")

            Spacer(minLength: 8)
            Button("Cancel") { develop.cancelCrop() }.accessibilityIdentifier("cropCancel")
            Button("Done") { develop.commitCrop() }
                .keyboardShortcut(.defaultAction)
                .accessibilityIdentifier("cropDone")
        }
        .controlSize(.small)
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(.regularMaterial)
    }
}
