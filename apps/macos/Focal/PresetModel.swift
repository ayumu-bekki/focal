import FocalCore
import Foundation

/// 現像のプリセット（v3.20、design.md 6.3 章）の状態。保存・適用・合成はすべて core が行う。
/// プリセットは現像の調整だけで、切り取り・回転・傾き補正は含まない
@MainActor
@Observable
final class PresetModel {
    private(set) var list: [PresetInfo] = []
    enum NamePrompt: Identifiable {
        case save
        case rename(PresetInfo)
        var id: String {
            switch self {
            case .save: "save"
            case .rename(let p): "rename-\(p.id)"
            }
        }
    }

    /// 名前を聞くシート（現像中の設定をプリセットとして保存する・名前を変える）
    var namePrompt: NamePrompt?
    /// いまの写真で最後に適用したプリセット。調整を動かして一致しなくなったら「変更あり」と上書きの操作を出す
    var appliedID: String?
    private(set) var store: PresetStore?
    /// プリセットの中身を書き換えた（上書きなど）世代。一覧の「いまの調整と同じか」の表示を作り直すために、ビューが読む
    private(set) var revision = 0
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

    /// いまの調整と同じ調整のプリセットの id（スライダーの操作のたびに呼んでよい）
    func match(_ settings: DevelopSettings) -> String? {
        _ = revision
        return store?.matching(settings)
    }

    func rename(_ preset: PresetInfo, to name: String) {
        guard let store, !preset.isBuiltIn else { return }
        do {
            try store.rename(id: preset.id, to: name)
            revision += 1
            reload()
        } catch { model?.report(error) }
    }

    /// 自分のプリセットを、いまの調整で上書きする
    func update(_ preset: PresetInfo) {
        guard !preset.isBuiltIn else { return }
        saveCurrent(name: preset.name)
        appliedID = preset.id  // 上書きしても、同じ名前のファイルなので id は変わらない
    }

    func name(of id: String?) -> String? { id.flatMap { id in list.first { $0.id == id }?.name } }

    /// いま現像している写真の調整を、この名前のプリセットにする（同じ名前があれば上書き）
    func saveCurrent(name: String) {
        guard let model, let store else { return }
        do {
            let id = try store.save(name: name, from: model.develop.settings)
            revision += 1
            reload()
            appliedID = id
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
                appliedID = preset.id
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
        Button("Save Settings as Preset…") { presets.namePrompt = .save }
            .disabled(!canSave)
        Menu("Delete Preset") {
            ForEach(presets.user) { p in
                Button(p.name, role: .destructive) { presets.delete(p) }
            }
        }
        .disabled(presets.user.isEmpty)
    }
}

/// プリセットの名前を聞くシート（保存・名前の変更）
struct PresetNameSheet: View {
    let presets: PresetModel
    let prompt: PresetModel.NamePrompt
    @State private var name = ""
    @Environment(\.dismiss) private var dismiss

    private var trimmed: String { name.trimmingCharacters(in: .whitespaces) }
    private var renaming: PresetInfo? {
        if case .rename(let p) = prompt { return p }
        return nil
    }
    /// 保存では、同じ名前の自分のプリセットを上書きする。名前の変更では、別のプリセットと同じ名前にはできない
    private var collides: Bool {
        presets.user.contains { $0.name.caseInsensitiveCompare(trimmed) == .orderedSame && $0.id != renaming?.id }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(renaming == nil ? "Save Settings as Preset" : "Rename Preset").font(.headline)
            if renaming == nil {
                Text("Only the adjustments are saved. Crop, rotation and straightening are not included.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(width: 320, alignment: .leading)
            }
            TextField("Name", text: $name)
                .frame(width: 320)
                .onSubmit(commit)
                .accessibilityIdentifier("presetName")
            if collides {
                Text(renaming == nil ? "A preset with this name already exists. It will be replaced."
                                     : "A preset with this name already exists.")
                    .font(.caption)
                    .foregroundStyle(.orange)
            }
            HStack {
                Spacer()
                Button("Cancel", role: .cancel) { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button(renaming == nil ? "Save" : "Rename", action: commit)
                    .keyboardShortcut(.defaultAction)
                    .disabled(trimmed.isEmpty || (renaming != nil && collides))
                    .accessibilityIdentifier("presetNameOK")
            }
        }
        .padding(20)
        .onAppear { if let p = renaming { name = p.name } }
    }

    private func commit() {
        guard !trimmed.isEmpty, !(renaming != nil && collides) else { return }
        if let p = renaming { presets.rename(p, to: trimmed) } else { presets.saveCurrent(name: trimmed) }
        dismiss()
    }
}
