import FocalCore
import SwiftUI

/// 右ペイン（9.1 章）。ビューアでは上に固定のヒストグラムと現像の区分、その下（区切り線の下）にメタデータとタグ
struct InspectorView: View {
    @Bindable var model: LibraryModel
    @State private var newTag = ""
    @AppStorage("inspector.metadata.expanded") private var metadataExpanded = true
    @AppStorage("inspector.tags.expanded") private var tagsExpanded = true

    static func kindName(_ kind: PhotoKind) -> String {
        switch kind {
        case .raw: "RAW"
        case .jpeg: "JPEG"
        case .tiff: "TIFF"
        case .png: "PNG"
        case .heif: "HEIF"
        }
    }

    private var developing: Bool { model.mode == .viewer && model.currentPhoto != nil }

    var body: some View {
        VStack(spacing: 0) {
            // ヒストグラムは上に固定（スクロールしても隠れない）
            if developing {
                HistogramHeader(develop: model.develop, presets: model.presets)
                Divider()
            }
            form
        }
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("inspector")
    }

    private var form: some View {
        Form {
            if developing {
                if model.develop.isEditable {
                    DevelopPanel(develop: model.develop, presets: model.presets)
                } else {
                    // RAW 以外（JPEG・TIFF・PNG・HEIF）は表示だけ（v3.22）
                    Section {
                        Text("Only RAW photos can be developed. You can still rate, flag, tag and organize this photo.")
                            .font(.callout)
                            .foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                            .accessibilityIdentifier("notDevelopable")
                    }
                }
            }
            if let p = model.currentPhoto {
                // ここから下は現像ではなく写真の情報（区切り線で分ける）
                Section {
                    if metadataExpanded {
                        row("File", p.fileName)
                        if p.kind != .raw { row("Format", Self.kindName(p.kind)) }
                        if !p.companions.isEmpty { row("Companion Files", p.companions.joined(separator: ", ")) }
                        LabeledContent("Rating") {
                            HStack(spacing: 2) {
                                ForEach(1...5, id: \.self) { i in
                                    Image(systemName: i <= p.rating ? "star.fill" : "star")
                                        .onTapGesture { model.setRating(i == p.rating ? 0 : i) }
                                }
                            }
                        }
                        // フラグ: ピック・なし・リジェクトから 1 つを選ぶ（排他な 3 状態なのでセグメントコントロール）
                        LabeledContent("Flag") {
                            Picker("Flag", selection: Binding(get: { p.flag }, set: { model.setFlag($0) })) {
                                Label("Picked", systemImage: "flag.fill").tag(PhotoFlag.picked)
                                Label("Unflagged", systemImage: "minus").tag(PhotoFlag.none)
                                Label("Rejected", systemImage: "xmark").tag(PhotoFlag.rejected)
                            }
                            .pickerStyle(.segmented)
                            .labelStyle(.iconOnly)
                            .labelsHidden()
                            .fixedSize()
                            .help("Pick (P), Unflagged (U), Reject (X)")
                            .accessibilityIdentifier("flagPicker")
                        }
                        if p.status != .ok {
                            row("Status", p.status == .missing ? String(localized: "File missing") : String(localized: "Unsupported"))
                                .foregroundStyle(.red)
                        }
                        row("Captured", p.captureTime?.replacingOccurrences(of: "T", with: " ") ?? "—")
                        row("Camera", "\(p.cameraMake) \(p.cameraModel)")
                        if !p.lensModel.isEmpty { row("Lens", p.lensModel) }
                        row("Exposure", Self.exposure(p))
                        row("Size", "\(p.width) × \(p.height)")
                    }
                } header: {
                    VStack(alignment: .leading, spacing: 8) {
                        if developing { Divider().padding(.top, 4) }
                        InspectorSectionHeader(title: "Metadata", expanded: $metadataExpanded, identifier: "metadataGroup")
                    }
                }
                Section {
                    if tagsExpanded {
                        ForEach(model.tags(ofPhoto: p.id)) { tag in
                            HStack {
                                Text(tag.path).lineLimit(1).truncationMode(.middle)
                                Spacer()
                                Button { model.removeTag(tag.id) } label: { Image(systemName: "xmark.circle.fill") }
                                    .buttonStyle(.borderless)
                            }
                        }
                        // 区分の見出しが「タグ」なのでラベルは出さず、何をする欄かを見本の文字で示す
                        TextField("Add Tag", text: $newTag, prompt: Text("Add tag (e.g. Travel/Kyoto)"))
                            .labelsHidden()
                            .help("Press Return to add. Use / to make nested tags (e.g. Travel/Kyoto).")
                            .accessibilityIdentifier("addTagField")
                            .onSubmit {
                                model.addTag(path: newTag)
                                newTag = ""
                            }
                    }
                } header: {
                    InspectorSectionHeader(title: "Tags", expanded: $tagsExpanded, identifier: "tagsGroup")
                }
            } else {
                Text("No photo selected").foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
    }

    private func row(_ label: LocalizedStringKey, _ value: String, id: String? = nil) -> some View {
        LabeledContent(label) {
            Text(value).lineLimit(1).truncationMode(.middle).help(value).textSelection(.enabled)
                .accessibilityIdentifier(id ?? "")
        }
    }

    static func exposure(_ p: Photo) -> String {
        var parts: [String] = []
        if let t = p.exposureTime { parts.append(t >= 1 ? String(format: "%.1fs", t) : "1/\(Int((1 / t).rounded()))") }
        if let f = p.fNumber { parts.append(String(format: "f/%.1f", f)) }
        if let iso = p.iso { parts.append("ISO \(iso)") }
        if let fl = p.focalLength { parts.append(String(format: "%.0fmm", fl)) }
        return parts.isEmpty ? "—" : parts.joined(separator: "  ")
    }
}
