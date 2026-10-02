import FocalCore
import SwiftUI

/// スマートアルバムの作成・条件の編集（v3.19）。条件を保存しておく読み取り専用のアルバムで、写真は足せない。
/// 条件は AND（すべて）か OR（いずれか）で結び、各行は「…でない」も指定できる。「アルバム X に含まれる」も条件にできる
struct SmartAlbumSheet: View {
    let model: LibraryModel
    let editing: Album?
    let parent: Int64?
    @State private var name = ""
    @State private var query = SmartQuery()
    @State private var error: String?
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(editing == nil ? "New Smart Album" : "Edit Smart Album").font(.headline)
            TextField("Name", text: $name)
                .accessibilityIdentifier("smartAlbumName")
            HStack {
                Text("Match")
                Picker("", selection: $query.matchAll) {
                    Text("all").tag(true)
                    Text("any").tag(false)
                }
                .labelsHidden()
                .fixedSize()
                .accessibilityIdentifier("smartMatch")
                Text("of the following rules:")
                Spacer()
            }
            VStack(spacing: 6) {
                ForEach($query.rules) { $rule in
                    HStack(alignment: .center, spacing: 6) {
                        SmartRuleRow(rule: $rule, model: model)
                        Button {
                            query.rules.removeAll { $0.id == rule.id }
                        } label: {
                            Image(systemName: "minus.circle")
                        }
                        .buttonStyle(.borderless)
                        .help("Remove this rule")
                        .accessibilityIdentifier("smartRemoveRule")
                    }
                }
                if query.rules.isEmpty {
                    Text("No rules: every photo matches.").font(.callout).foregroundStyle(.secondary)
                }
                HStack {
                    Button {
                        query.rules.append(SmartRule())
                    } label: {
                        Label("Add Rule", systemImage: "plus.circle")
                    }
                    .buttonStyle(.borderless)
                    .accessibilityIdentifier("smartAddRule")
                    Spacer()
                }
            }
            .frame(minHeight: 60)
            if let error {
                Text(error).font(.caption).foregroundStyle(.red)
            }
            HStack {
                Text("A smart album only shows photos that match. You cannot add photos to it.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Spacer()
                Button("Cancel", role: .cancel) { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button(editing == nil ? "Create" : "Save", action: commit)
                    .keyboardShortcut(.defaultAction)
                    .disabled(name.trimmingCharacters(in: .whitespaces).isEmpty)
                    .accessibilityIdentifier("smartAlbumOK")
            }
        }
        .padding(20)
        .frame(width: 600)
        .onAppear {
            if let a = editing {
                name = a.name
                query = model.smartQuery(of: a)
            } else {
                name = model.untitledSmartAlbumName()
                query.rules = [SmartRule()]
            }
        }
    }

    private func commit() {
        let n = name.trimmingCharacters(in: .whitespaces)
        guard !n.isEmpty else { return }
        do {
            try model.saveSmartAlbum(editing: editing, name: n, queryJSON: query.jsonString, parent: parent)
            dismiss()
        } catch {
            self.error = (error as? FocalError)?.message ?? String(describing: error)
        }
    }
}

/// 条件の 1 行: 項目 · 演算子 · 値
private struct SmartRuleRow: View {
    @Binding var rule: SmartRule
    let model: LibraryModel

    var body: some View {
        HStack(spacing: 6) {
            Picker("", selection: Binding(get: { rule.field }, set: { rule.setField($0) })) {
                ForEach(SmartRule.Field.allCases) { Text($0.title).tag($0) }
            }
            .labelsHidden()
            .frame(width: 140)
            .accessibilityIdentifier("smartField")
            Picker("", selection: $rule.op) {
                ForEach(rule.field.operators, id: \.op) { Text($0.title).tag($0.op) }
            }
            .labelsHidden()
            .frame(width: 140)
            .accessibilityIdentifier("smartOp")
            value
            Spacer(minLength: 0)
        }
    }

    @ViewBuilder private var value: some View {
        switch rule.field {
        case .rating:
            Picker("", selection: Binding(get: { Int(rule.number) }, set: { rule.number = Double($0) })) {
                ForEach(0...5, id: \.self) { Text($0 == 0 ? String(localized: "No Rating") : String(repeating: "★", count: $0)).tag($0) }
            }
            .labelsHidden()
            .fixedSize()
        case .flag:
            Picker("", selection: $rule.flag) {
                Text("Picked").tag("pick")
                Text("Rejected").tag("reject")
                Text("Unflagged").tag("none")
            }
            .labelsHidden()
            .fixedSize()
        case .tag:
            Picker("", selection: $rule.itemID) {
                if rule.itemID == 0 { Text("Choose…").tag(Int64(0)) }
                ForEach(model.tags) { Text($0.path).tag($0.id) }
            }
            .labelsHidden()
            .onAppear { if rule.itemID == 0, let first = model.tags.first { rule.itemID = first.id } }
        case .album:
            Picker("", selection: $rule.itemID) {
                if rule.itemID == 0 { Text("Choose…").tag(Int64(0)) }
                // 手で集めるアルバムとフォルダ（フォルダなら中のアルバムも含む）
                ForEach(model.albums.filter { $0.kind != .smart }) { Text($0.name).tag($0.id) }
            }
            .labelsHidden()
            .onAppear { if rule.itemID == 0, let first = model.albums.first(where: { $0.kind != .smart }) { rule.itemID = first.id } }
        case .camera, .lens, .fileName:
            TextField("", text: $rule.text)
                .accessibilityIdentifier("smartText")
        case .iso, .focalLength, .fNumber:
            TextField("", value: $rule.number, format: .number)
                .frame(width: 90)
                .accessibilityIdentifier("smartNumber")
        case .date:
            HStack {
                DatePicker("", selection: $rule.date, displayedComponents: .date).labelsHidden()
                if rule.op == "between" {
                    Text("and")
                    DatePicker("", selection: $rule.dateEnd, displayedComponents: .date).labelsHidden()
                }
            }
        }
    }
}
