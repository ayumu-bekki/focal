import FocalCore
import SwiftUI

/// 絞り込みバー（9.5 章）。ツールバーの「絞り込み」ボタンで開閉する。
/// 評価・フラグはメニュー、撮影日・フォルダ・タグはポップオーバーで選ぶ
struct FilterBar: View {
    @Bindable var model: LibraryModel

    var body: some View {
        HStack(spacing: 14) {
            Picker("Rating", selection: $model.filter.minRating) {
                Text("Any").tag(0)
                ForEach(1...5, id: \.self) { Text("≥ " + String(repeating: "★", count: $0)).tag($0) }
            }
            .fixedSize()
            .accessibilityIdentifier("filterRating")

            Picker("Flag", selection: $model.filter.flag) {
                Text("Any").tag(PhotoFilter.Flag.any)
                Text("Picked").tag(PhotoFilter.Flag.picked)
                Text("Not rejected").tag(PhotoFilter.Flag.notRejected)
                Text("Unflagged").tag(PhotoFilter.Flag.unflagged)
                Text("Rejected").tag(PhotoFilter.Flag.rejected)
            }
            .fixedSize()
            .accessibilityIdentifier("filterFlag")

            PopoverField(title: "Capture date", value: dateSummary, identifier: "filterDate") {
                DateFilterPanel(model: model)
            }
            PopoverField(title: "Folder", value: model.filter.folderID.flatMap { LibraryModel.find($0, in: model.folderTree)?.name },
                         identifier: "filterFolder") { dismiss in
                TreePicker(nodes: model.folderTree.map(TreeItem.init), systemImage: "folder",
                           allTitle: "All Folders", selection: model.filter.folderID) { id in
                    model.filter.folderID = id
                    dismiss()
                }
            }
            PopoverField(title: "Tag", value: model.filter.tagID.flatMap { LibraryModel.find($0, in: model.tagTree)?.name },
                         identifier: "filterTag") { dismiss in
                TreePicker(nodes: model.tagTree.map(TreeItem.init), systemImage: "tag",
                           allTitle: "All Tags", selection: model.filter.tagID) { id in
                    model.filter.tagID = id
                    dismiss()
                }
            }

            Spacer(minLength: 0)
            if model.isFiltering {
                Button("Clear") { model.clearFilters() }
                    .accessibilityIdentifier("filterClear")
            }
        }
        .controlSize(.small)
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .background(.bar)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("filterBar")
    }

    private var dateSummary: String? {
        let f = model.filter
        guard f.dateFrom != nil || f.dateTo != nil else { return nil }
        return "\(f.dateFrom ?? "")〜\(f.dateTo ?? "")"
    }
}

/// 「名前: 値 ▾」のボタン。押すとポップオーバーで中身を出す
private struct PopoverField<Content: View>: View {
    let title: LocalizedStringKey
    let value: String?
    let identifier: String
    @ViewBuilder let content: (_ dismiss: @escaping () -> Void) -> Content
    @State private var shown = false

    init(title: LocalizedStringKey, value: String?, identifier: String,
         @ViewBuilder content: @escaping (_ dismiss: @escaping () -> Void) -> Content) {
        self.title = title
        self.value = value
        self.identifier = identifier
        self.content = content
    }

    init(title: LocalizedStringKey, value: String?, identifier: String, @ViewBuilder content: @escaping () -> Content) {
        self.init(title: title, value: value, identifier: identifier) { _ in content() }
    }

    var body: some View {
        HStack(spacing: 4) {
            Text(title)
            Button {
                shown.toggle()
            } label: {
                HStack(spacing: 2) {
                    Text(value ?? String(localized: "Any")).lineLimit(1).truncationMode(.middle)
                    Image(systemName: "chevron.down").font(.caption2)
                }
                .frame(maxWidth: 160)
            }
            .accessibilityIdentifier(identifier)
            .popover(isPresented: $shown, arrowEdge: .bottom) {
                content { shown = false }
                    .padding(10)
            }
        }
    }
}

/// 撮影日の範囲（"YYYY-MM-DD"）
private struct DateFilterPanel: View {
    @Bindable var model: LibraryModel

    private static let formatter: DateFormatter = {
        let f = DateFormatter()
        f.calendar = Calendar(identifier: .gregorian)
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy-MM-dd"
        return f
    }()

    private var enabled: Binding<Bool> {
        Binding(get: { model.filter.dateFrom != nil || model.filter.dateTo != nil },
                set: { on in
                    if on {
                        let from = Calendar.current.date(byAdding: .year, value: -1, to: .now)!
                        model.filter.dateFrom = Self.formatter.string(from: from)
                        model.filter.dateTo = Self.formatter.string(from: .now)
                    } else {
                        model.filter.dateFrom = nil
                        model.filter.dateTo = nil
                    }
                })
    }

    private func date(_ key: WritableKeyPath<PhotoFilter, String?>) -> Binding<Date> {
        Binding(get: { model.filter[keyPath: key].flatMap { Self.formatter.date(from: $0) } ?? .now },
                set: { model.filter[keyPath: key] = Self.formatter.string(from: $0) })
    }

    var body: some View {
        Form {
            Toggle("Filter by capture date", isOn: enabled)
            DatePicker("From", selection: date(\.dateFrom), displayedComponents: .date)
                .disabled(!enabled.wrappedValue)
            DatePicker("To", selection: date(\.dateTo), displayedComponents: .date)
                .disabled(!enabled.wrappedValue)
        }
        .frame(width: 260)
    }
}

/// フォルダ・タグの階層（ポップオーバーの中で選ぶ）
private struct TreeItem: Identifiable, Hashable {
    let id: Int64
    let name: String
    let count: Int64
    var children: [TreeItem]?

    init(_ n: FolderNode) {
        id = n.id
        name = n.name
        count = n.photoCount
        children = n.children?.map(TreeItem.init)
    }

    init(_ n: TagNode) {
        id = n.id
        name = n.name
        count = n.photoCount
        children = n.children?.map(TreeItem.init)
    }
}

private struct TreePicker: View {
    let nodes: [TreeItem]
    let systemImage: String
    let allTitle: LocalizedStringKey
    let selection: Int64?
    let choose: (Int64?) -> Void

    var body: some View {
        List {
            row(title: Text(allTitle), image: "square.stack", count: nil, selected: selection == nil) { choose(nil) }
            OutlineGroup(nodes, children: \.children) { n in
                row(title: Text(n.name), image: systemImage, count: n.count, selected: selection == n.id) { choose(n.id) }
            }
        }
        .listStyle(.sidebar)
        .frame(width: 280, height: 320)
    }

    private func row(title: Text, image: String, count: Int64?, selected: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack {
                Label { title.lineLimit(1).truncationMode(.middle) } icon: { Image(systemName: image) }
                Spacer(minLength: 4)
                if let count { Text("\(count)").foregroundStyle(.secondary).monospacedDigit() }
                if selected { Image(systemName: "checkmark").foregroundStyle(.tint) }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}
