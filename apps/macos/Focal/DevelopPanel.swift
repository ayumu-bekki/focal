import FocalCore
import SwiftUI

/// インスペクタの上に固定するヒストグラム（9.1 章）。スクロールしても隠れない
struct HistogramHeader: View {
    let develop: DevelopModel
    let presets: PresetModel

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HistogramView(histogram: develop.histogram)
            DevelopTools(develop: develop, presets: presets)
            if develop.stage != .ready {
                HStack(spacing: 6) {
                    ProgressView().controlSize(.mini)
                    Text(develop.stage == .failed ? "Cannot open this photo" : "Loading RAW…")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .accessibilityIdentifier("developLoading")
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
    }
}

/// 現像の操作（9.5 章）: Fit / 100%、切り抜き、90° 回転。ヒストグラムの下に固定する
struct DevelopTools: View {
    @Bindable var develop: DevelopModel
    let presets: PresetModel

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            toolRow
            if develop.syncTargetCount > 0 || develop.autoSync { autoSyncRow }
        }
        .disabled(develop.stage != .ready)
    }

    /// 現像パラメータの同期（Lightroom Classic の Auto Sync）: 選択中の他の写真があるときだけ出す。オンの間、動かした項目だけが他の写真に写る
    private var autoSyncRow: some View {
        HStack(spacing: 6) {
            Toggle("Sync Develop Settings", isOn: $develop.autoSync)
                .toggleStyle(.switch)
                .controlSize(.mini)
                .disabled(!develop.isEditable || develop.syncTargetCount == 0)
                .help("Apply only the adjustments you change to the other selected photos too")
                .accessibilityIdentifier("autoSyncToggle")
            if develop.autoSync {
                Text("Syncing to \(develop.syncTargetCount) Other Photos")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .accessibilityIdentifier("autoSyncCount")
            }
            Spacer(minLength: 0)
        }
        .font(.caption)
    }

    private var toolRow: some View {
        HStack(spacing: 8) {
            Picker("Zoom", selection: Binding(
                get: { develop.zoom == .fit },
                set: { fit in if fit != (develop.zoom == .fit) { develop.toggleZoom(at: nil) } })) {
                Text("Fit").tag(true)
                Text("100%").tag(false)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .fixedSize()
            .disabled(develop.stage != .ready || develop.cropMode)
            .accessibilityIdentifier("zoomPicker")
            Spacer(minLength: 0)
            Toggle(isOn: Binding(get: { develop.cropMode }, set: { _ in develop.toggleCropMode() })) {
                Label("Crop", systemImage: "crop")
                    .labelStyle(.iconOnly)
            }
            .toggleStyle(.button)
            .disabled(!develop.isEditable)
            .help("Crop (C)")
            .accessibilityIdentifier("cropButton")
            Menu {
                PresetMenuItems(presets: presets, canSave: develop.stage == .ready)
            } label: {
                Image(systemName: "slider.horizontal.2.square")
            }
            .menuStyle(.borderlessButton)
            .menuIndicator(.hidden)
            .fixedSize()
            .disabled(develop.stage != .ready || develop.cropMode || !develop.isEditable)
            .help("Presets")
            .accessibilityLabel("Presets")
            .accessibilityIdentifier("presetsMenu")
            Button { develop.rotate(by: -1) } label: { Image(systemName: "rotate.left") }
                .disabled(!develop.isEditable)
                .help("Rotate Left ([)")
                .accessibilityIdentifier("rotateLeftButton")
            Button { develop.rotate(by: 1) } label: { Image(systemName: "rotate.right") }
                .disabled(!develop.isEditable)
                .help("Rotate Right (])")
                .accessibilityIdentifier("rotateRightButton")
        }
        .controlSize(.small)
    }
}

/// インスペクタの現像パネル（9.1 章）: WB、ライト（露出〜明るさ）、カラー（彩度・自然な彩度）、ディテール（明瞭度・シャープネス・ノイズ低減）。
/// 区分ごとに開閉でき、開閉の状態は次回の起動でも保つ
struct DevelopPanel: View {
    @Bindable var develop: DevelopModel
    let presets: PresetModel
    @AppStorage("inspector.presets.expanded") private var presetsExpanded = true
    @AppStorage("inspector.whiteBalance.expanded") private var wbExpanded = true
    @AppStorage("inspector.light.expanded") private var lightExpanded = true
    @AppStorage("inspector.color.expanded") private var colorExpanded = true
    @AppStorage("inspector.detail.expanded") private var detailExpanded = true
    @AppStorage("inspector.lens.expanded") private var lensExpanded = false
    @State private var lensPicker = false

    var body: some View {
        // 読み込み中は中身だけを操作できなくする（見出しは開閉できる）
        let disabled = develop.stage != .ready
        presetSection(disabled: disabled)

        Section {
            if wbExpanded {
                Group {
                    Picker("White Balance", selection: Binding(
                        get: { develop.settings.customWhiteBalance },
                        set: { develop.setCustomWhiteBalance($0) })) {
                        Text("As Shot").tag(false)
                        Text("Custom").tag(true)
                    }
                    .controlSize(.small)
                    .accessibilityIdentifier("wbMode")
                    // 撮影時の設定でも動かせる（動かすと撮影時の値から始めてカスタムになる）。ダブルクリックで撮影時の設定に戻す
                    SliderRow(title: "Temperature", valueText: "\(Int(displayTemperature.rounded())) K",
                              value: temperatureSlider, range: 0...1, defaultValue: Self.miredPosition(5500),
                              identifier: "temperatureSlider", develop: develop,
                              onReset: { develop.setCustomWhiteBalance(false) })
                    SliderRow(title: "Tint", valueText: String(format: "%+.0f", displayTint),
                              value: Binding(get: { displayTint },
                                             set: { v in develop.updateWhiteBalance { $0.tint = v.rounded() } }),
                              range: -150...150, defaultValue: 0, identifier: "tintSlider", develop: develop,
                              onReset: { develop.setCustomWhiteBalance(false) })
                }
                .disabled(disabled)
            }
        } header: {
            InspectorSectionHeader(title: "White Balance", expanded: $wbExpanded, identifier: "whiteBalanceGroup") {
                develop.resetWhiteBalance()
            }
        }

        Section {
            if lightExpanded {
                Group {
                    SliderRow(title: "Exposure", valueText: String(format: "%+.2f EV", develop.settings.exposure),
                              value: bind(\.exposure), range: -5...5, defaultValue: 0, identifier: "exposureSlider",
                              develop: develop)
                    amountRow("Contrast", \.contrast, "contrastSlider")
                    amountRow("Highlights", \.highlights, "highlightsSlider")
                    amountRow("Shadows", \.shadows, "shadowsSlider")
                    amountRow("Whites", \.whites, "whitesSlider")
                    amountRow("Blacks", \.blacks, "blacksSlider")
                    amountRow("Brightness", \.brightness, "brightnessSlider")
                }
                .disabled(disabled)
            }
        } header: {
            InspectorSectionHeader(title: "Light", expanded: $lightExpanded, identifier: "lightGroup") {
                develop.resetLight()
            }
        }

        Section {
            if colorExpanded {
                Group {
                    amountRow("Saturation", \.saturation, "saturationSlider")
                    amountRow("Vibrance", \.vibrance, "vibranceSlider")
                }
                .disabled(disabled)
            }
        } header: {
            InspectorSectionHeader(title: "Color", expanded: $colorExpanded, identifier: "colorGroup") {
                develop.resetColor()
            }
        }

        Section {
            if detailExpanded {
                Group {
                    amountRow("Clarity", \.clarity, "claritySlider", limit: 200)
                    amountRow("Sharpness", \.sharpness, "sharpnessSlider", range: 0...150)
                    amountRow("Luma Noise", \.noiseReduction, "noiseReductionSlider", range: 0...100)
                    amountRow("Color Noise", \.colorNoiseReduction, "colorNoiseReductionSlider", range: 0...100)
                }
                .disabled(disabled)
                // 半径が 1〜数画素の処理なので、縮小表示ではほとんど見えない
                Text("Sharpness and noise reduction are visible at 100%.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        } header: {
            InspectorSectionHeader(title: "Detail", expanded: $detailExpanded, identifier: "detailGroup") {
                develop.resetDetail()
            }
        }

        if LensLibrary.isSupported { lensSection(disabled: disabled) }
    }

    // MARK: レンズ補正（v3.27）

    private static let projections: [(Int32, LocalizedStringKey)] = [
        (0, "Same as lens"), (1, "Rectilinear"), (2, "Fisheye"), (3, "Equisolid fisheye"),
        (4, "Stereographic fisheye"), (5, "Orthographic fisheye"), (6, "Panoramic"), (7, "Equirectangular"),
    ]

    @ViewBuilder private func lensSection(disabled: Bool) -> some View {
        let s = develop.settings
        Section {
            if lensExpanded {
                Group {
                    Toggle("Enable Lens Correction", isOn: Binding(get: { develop.settings.lensEnabled },
                                                                    set: { v in develop.update { $0.lensEnabled = v } }))
                        .accessibilityIdentifier("lensToggle")
                    lensName(s)
                    Group {
                        amountRow("Distortion", \.lensDistortion, "lensDistortionSlider", range: 0...100, defaultValue: 100)
                        amountRow("Chromatic Ab.", \.lensTCA, "lensTCASlider", range: 0...100, defaultValue: 100)
                        amountRow("Vignetting", \.lensVignetting, "lensVignettingSlider", range: 0...100, defaultValue: 100)
                        Picker("Projection", selection: Binding(get: { develop.settings.lensProjection },
                                                                set: { v in develop.update { $0.lensProjection = v } })) {
                            ForEach(Self.projections, id: \.0) { Text($0.1).tag($0.0) }
                        }
                        .accessibilityIdentifier("lensProjectionPicker")
                    }
                    .disabled(!s.lensEnabled)
                }
                .disabled(disabled)
                Text("Lens data: Lensfun (CC BY-SA 3.0). See Licenses in the Focal menu.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        } header: {
            InspectorSectionHeader(title: "Lens Correction", expanded: $lensExpanded, identifier: "lensGroup") {
                develop.resetLens()
            }
        }
    }

    /// 使うレンズ。自動のときは写真のレンズ名から選ばれたもの（なければその旨）。「選ぶ…」で手動に切り替える
    @ViewBuilder private func lensName(_ s: DevelopSettings) -> some View {
        let manual = s.lensID.isEmpty ? nil : LensLibrary.find(id: s.lensID)
        let detected = develop.detectedLens
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Group {
                    if !s.lensID.isEmpty {
                        Text(manual?.displayName ?? s.lensID)
                    } else if let detected {
                        Text("Auto: \(detected.displayName)")
                    } else {
                        Text("Auto: no matching lens")
                            .foregroundStyle(.secondary)
                    }
                }
                .accessibilityIdentifier("lensName")
                Spacer(minLength: 0)
                Button("Choose…") { lensPicker = true }
                    .controlSize(.small)
                    .accessibilityIdentifier("lensChooseButton")
                    .popover(isPresented: $lensPicker, arrowEdge: .leading) {
                        LensPicker(develop: develop, dismiss: { lensPicker = false })
                    }
            }
            .font(.callout)
            .lineLimit(2)
            let exif = develop.lensExif.replacingOccurrences(of: "\n", with: "  ")
            if !exif.isEmpty {
                Text(exif).font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
        }
    }

    /// 既定 0 のスライダー。範囲は -limit〜+limit（明瞭度は ±200）か、range で指定（シャープネス・ノイズ低減は 0 から）
    private func amountRow(_ title: LocalizedStringKey, _ key: WritableKeyPath<DevelopSettings, Double>,
                           _ identifier: String, limit: Double = 100,
                           range: ClosedRange<Double>? = nil, defaultValue: Double = 0) -> some View {
        let r = range ?? -limit...limit
        let v = develop.settings[keyPath: key]
        return SliderRow(title: title, valueText: r.lowerBound < 0 ? String(format: "%+.0f", v) : String(format: "%.0f", v),
                         value: bind(key), range: r, defaultValue: defaultValue, identifier: identifier, develop: develop)
    }

    private func bind(_ key: WritableKeyPath<DevelopSettings, Double>) -> Binding<Double> {
        Binding(get: { develop.settings[keyPath: key] }, set: { v in develop.update { $0[keyPath: key] = v } })
    }

    // 色温度は逆数（mired）で等間隔にする（低い色温度ほど細かく調整できる）
    private static let minK = 2000.0, maxK = 50000.0
    static func miredPosition(_ k: Double) -> Double {
        (1e6 / minK - 1e6 / k) / (1e6 / minK - 1e6 / maxK)
    }
    static func kelvin(_ position: Double) -> Double {
        1e6 / (1e6 / minK - position * (1e6 / minK - 1e6 / maxK))
    }

    private var displayTemperature: Double {
        develop.settings.customWhiteBalance ? develop.settings.temperature : (develop.info?.asShotTemperature ?? 0)
    }

    private var displayTint: Double {
        develop.settings.customWhiteBalance ? develop.settings.tint : (develop.info?.asShotTint ?? 0)
    }

    // MARK: プリセット（v3.21）

    /// プリセットの一覧。クリックで適用、いまの調整と同じプリセットにはチェックを付ける。調整を動かすと「変更あり」と、
    /// 自分のプリセットなら「更新」（上書き）を出す。右クリックで名前の変更・削除、見出しの ＋ でいまの調整を保存
    @ViewBuilder private func presetSection(disabled: Bool) -> some View {
        Section {
            if presetsExpanded {
                let match = disabled ? nil : presets.match(develop.settings)
                Group {
                    presetRow(title: Text("None"), selected: develop.settings.hasNoAdjustments, modified: false,
                              identifier: "presetRow-none", update: nil) { develop.resetAdjustments() }
                    ForEach(presets.builtIn) { p in
                        presetRow(preset: p, match: match)
                    }
                    ForEach(presets.user) { p in
                        presetRow(preset: p, match: match)
                            .contextMenu {
                                Button("Rename…") { presets.namePrompt = .rename(p) }
                                Button("Delete", role: .destructive) { presets.delete(p) }
                            }
                    }
                }
                .disabled(disabled)
            }
        } header: {
            InspectorSectionHeader(title: "Presets", expanded: $presetsExpanded, identifier: "presetsGroup")
                .overlay(alignment: .trailing) {
                    Button {
                        presets.namePrompt = .save
                    } label: {
                        Image(systemName: "plus")
                    }
                    .buttonStyle(.borderless)
                    .help("Save Settings as Preset…")
                    .accessibilityLabel("Save Settings as Preset…")
                    .accessibilityIdentifier("presetAdd")
                    .disabled(disabled)
                }
        }
        .onChange(of: develop.photoID) { presets.appliedID = nil }
    }

    private func presetRow(preset: PresetInfo, match: String?) -> some View {
        let selected = match == preset.id
        let modified = !selected && presets.appliedID == preset.id
        return presetRow(title: Text(preset.name), selected: selected, modified: modified,
                         identifier: "presetRow-\(preset.name)",
                         update: modified && !preset.isBuiltIn ? { presets.update(preset) } : nil) {
            presets.apply(preset)
        }
    }

    private func presetRow(title: Text, selected: Bool, modified: Bool, identifier: String,
                           update: (() -> Void)?, action: @escaping () -> Void) -> some View {
        HStack(spacing: 6) {
            Image(systemName: "checkmark")
                .font(.caption.weight(.semibold))
                .frame(width: 12)
                .opacity(selected ? 1 : 0)
            Button(action: action) {
                HStack(spacing: 6) {
                    title.lineLimit(1).truncationMode(.tail)
                    if modified {
                        Text("Modified").font(.caption).foregroundStyle(.secondary)
                    }
                    Spacer(minLength: 0)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityIdentifier(identifier)
            .accessibilityValue(selected ? "selected" : "")
            if let update {
                Button("Update", action: update)
                    .buttonStyle(.borderless)
                    .font(.caption)
                    .help("Overwrite this preset with the current adjustments")
                    .accessibilityIdentifier("\(identifier)-update")
            }
        }
        .font(.callout)
    }

    private var temperatureSlider: Binding<Double> {
        Binding(get: { Self.miredPosition(max(Self.minK, displayTemperature)) },
                set: { p in develop.updateWhiteBalance { $0.temperature = (Self.kelvin(p) / 10).rounded() * 10 } })
    }
}

/// インスペクタの区分の見出し（全区分で共通）。クリックで開閉、reset があれば右端にリセット。
/// フォント・色をここで決めて、現像の区分とメタデータ・タグで見た目をそろえる
struct InspectorSectionHeader: View {
    let title: LocalizedStringKey
    @Binding var expanded: Bool
    let identifier: String
    var reset: (() -> Void)? = nil

    static let font = Font.subheadline.weight(.semibold)

    var body: some View {
        HStack(spacing: 4) {
            Button {
                withAnimation(.easeInOut(duration: 0.15)) { expanded.toggle() }
            } label: {
                HStack(spacing: 5) {
                    Image(systemName: "chevron.right")
                        .font(.caption2.weight(.bold))
                        .rotationEffect(.degrees(expanded ? 90 : 0))
                        .frame(width: 10)
                    Text(title)
                    Spacer(minLength: 0)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(HeaderButtonStyle())
            .accessibilityIdentifier(identifier)
            .accessibilityValue(expanded ? "expanded" : "collapsed")
            if let reset {
                Button("Reset", action: reset)
                    .buttonStyle(HeaderButtonStyle())
                    .font(.subheadline)
            }
        }
        .font(Self.font)
        .foregroundStyle(.secondary)
        .textCase(nil)
    }
}

/// 見出しのボタン: 押している間だけ薄くする（離した後やカーソルが乗っただけでは変えない）
private struct HeaderButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label.opacity(configuration.isPressed ? 0.5 : 1)
    }
}

/// 名前・スライダー・値を 1 行に並べる（縦に詰める）
private struct SliderRow: View {
    let title: LocalizedStringKey
    let valueText: String
    @Binding var value: Double
    let range: ClosedRange<Double>
    let defaultValue: Double
    let identifier: String
    let develop: DevelopModel
    var onReset: (() -> Void)? = nil

    var body: some View {
        HStack(spacing: 6) {
            Text(title)
                .lineLimit(1)
                .minimumScaleFactor(0.8)
                .frame(width: 78, alignment: .leading)
            DevSlider(value: $value, range: range, defaultValue: defaultValue, identifier: identifier,
                      onBegin: { develop.beginChange() }, onEnd: { develop.endChange() }, onReset: onReset)
                .baselineAtCenter()
            Text(valueText)
                .monospacedDigit()
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .frame(width: 58, alignment: .trailing)
        }
        .font(.callout)
    }
}

/// レンズを探して選ぶ（v3.27）。検索は core（Lensfun の DB）。「自動」で写真のレンズ名からの選択に戻す
private struct LensPicker: View {
    let develop: DevelopModel
    let dismiss: () -> Void
    @State private var query = ""
    @State private var results: [LensInfo] = []
    @State private var counts = LensLibrary.counts()

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            TextField("Search lenses", text: $query)
                .textFieldStyle(.roundedBorder)
                .accessibilityIdentifier("lensSearchField")
                .onChange(of: query) { _, q in results = LensLibrary.search(q, limit: 100) }
            List(results) { lens in
                Button {
                    develop.update { $0.lensID = lens.id; $0.lensEnabled = true }
                    dismiss()
                } label: {
                    VStack(alignment: .leading, spacing: 1) {
                        Text(lens.displayName)
                        Text(lens.mounts).font(.caption).foregroundStyle(.secondary)
                    }
                }
                .buttonStyle(.plain)
            }
            .frame(width: 320, height: 240)
            .accessibilityIdentifier("lensResults")
            HStack {
                Button("Auto") {
                    develop.update { $0.lensID = "" }
                    dismiss()
                }
                .accessibilityIdentifier("lensAutoButton")
                Spacer()
                Text("\(counts.lenses) lenses")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(12)
        .onAppear {
            counts = LensLibrary.counts()
            results = LensLibrary.search("", limit: 100)
        }
    }
}
