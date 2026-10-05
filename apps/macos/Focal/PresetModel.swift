import FocalCore
import Foundation

/// 現像のプリセット（v3.20、design.md 6.3 章）の状態。保存・適用・合成はすべて core が行う。
/// プリセットは現像の調整だけで、切り取り・回転・傾き補正は含まない
@MainActor
@Observable
final class PresetModel {
    private(set) var list: [PresetInfo] = []
    /// 名前を聞くシートを出す（現像中の設定をプリセットとして保存する）
    var showNamePrompt = false
    private(set) var store: PresetStore?
    private weak var model: LibraryModel?

    var builtIn: [PresetInfo] { list.filter(\.isBuiltIn) }
    var user: [PresetInfo] { list.filter { !$0.isBuiltIn } }

    func configure(model: LibraryModel) {
        self.model = model
        do {
            let dir = AppPaths.presetsDirectory
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            store = try PresetStore(userDirectory: dir, builtInDirectory: AppPaths.builtInPresetsDirectory)
            reload()
        } catch {
            model.report(error)
        }
    }

    func reload() {
        guard let store else { return }
        do {
            let l = try store.list()
            if l != list { list = l }
        } catch { model?.report(error) }
    }

    func name(of id: String?) -> String? { id.flatMap { id in list.first { $0.id == id }?.name } }

    /// いま現像している写真の調整を、この名前のプリセットにする（同じ名前があれば上書き）
    func saveCurrent(name: String) {
        guard let model, let store else { return }
        do {
            try store.save(name: name, from: model.develop.settings)
            reload()
        } catch { model.report(error) }
    }

    func delete(_ preset: PresetInfo) {
        guard let store, !preset.isBuiltIn else { return }
        do {
            try store.delete(id: preset.id)
            reload()
            if model?.cardImport.presetID == preset.id { model?.cardImport.presetID = nil }
        } catch { model?.report(error) }
    }

    /// 現像画面なら開いている写真に、一覧なら選択中の写真に、プリセットの調整を重ねる
    func apply(_ preset: PresetInfo) {
        guard let model, let store else { return }
        do {
            if model.mode == .viewer {
                try model.develop.applyPreset(preset.id, using: store)
                return
            }
            var ids = model.targetIDs
            // 一覧に戻ったあとも開いたままの写真は、セッションの設定を変える（あとで保存が上書きしないように）
            if let open = model.develop.photoID, let i = ids.firstIndex(of: open) {
                try model.develop.applyPreset(preset.id, using: store)
                ids.remove(at: i)
            }
            guard !ids.isEmpty else { return }
            let catalog = model.catalog
            let targets = ids
            Task {
                // 書き込みが終わってから、セルを読み直す（サムネイルは編集の内容で作り直される）
                let failure: String? = await Task.detached {
                    do {
                        try catalog.applyPreset(id: preset.id, from: store, to: targets)
                        try catalog.flush()
                        return nil
                    } catch { return String(describing: error) }
                }.value
                if let failure { model.reportMessage(failure) } else { model.afterPhotoChange(targets) }
            }
        } catch { model.report(error) }
    }
}

import SwiftUI

/// プリセットのメニューの中身（現像の操作・写真メニューで共通）。選ぶと開いている写真（一覧では選択中の写真）に重ねる
struct PresetMenuItems: View {
    let presets: PresetModel
    /// 現像中の設定を保存できるか（現像画面で写真が開いているとき）
    let canSave: Bool

    var body: some View {
        if presets.list.isEmpty {
            Text("No Presets")
        }
        ForEach(presets.builtIn) { p in
            Button(p.name) { presets.apply(p) }
        }
        if !presets.builtIn.isEmpty && !presets.user.isEmpty { Divider() }
        ForEach(presets.user) { p in
            Button(p.name) { presets.apply(p) }
        }
        Divider()
        Button("Save Settings as Preset…") { presets.showNamePrompt = true }
            .disabled(!canSave)
        Menu("Delete Preset") {
            ForEach(presets.user) { p in
                Button(p.name, role: .destructive) { presets.delete(p) }
            }
        }
        .disabled(presets.user.isEmpty)
    }
}

/// プリセットの名前を聞くシート
struct PresetNameSheet: View {
    let presets: PresetModel
    @State private var name = ""
    @Environment(\.dismiss) private var dismiss

    private var trimmed: String { name.trimmingCharacters(in: .whitespaces) }
    private var overwrites: Bool { presets.user.contains { $0.name.caseInsensitiveCompare(trimmed) == .orderedSame } }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Save Settings as Preset").font(.headline)
            Text("Only the adjustments are saved. Crop, rotation and straightening are not included.")
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
                .frame(width: 320, alignment: .leading)
            TextField("Name", text: $name)
                .frame(width: 320)
                .onSubmit(commit)
                .accessibilityIdentifier("presetName")
            if overwrites {
                Text("A preset with this name already exists. It will be replaced.")
                    .font(.caption)
                    .foregroundStyle(.orange)
            }
            HStack {
                Spacer()
                Button("Cancel", role: .cancel) { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button("Save", action: commit)
                    .keyboardShortcut(.defaultAction)
                    .disabled(trimmed.isEmpty)
                    .accessibilityIdentifier("presetNameOK")
            }
        }
        .padding(20)
    }

    private func commit() {
        guard !trimmed.isEmpty else { return }
        presets.saveCurrent(name: trimmed)
        dismiss()
    }
}
