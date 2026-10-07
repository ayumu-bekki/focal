import AppKit
import FocalCore
import SwiftUI

/// 取り込むカードの写真の一覧（v3.29、design.md 5.10 章）。チェックした写真だけを取り込む。取り込み済みの写真は薄く出して
/// チェックできない（既定では隠す）。撮影日時順。⇧クリックで範囲をそろえてチェック・解除する
struct CardShotGrid: View {
    @Bindable var importer: CardImportModel
    /// ⇧クリックの起点
    @State private var anchor: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            header
            ZStack {
                if importer.isListing {
                    VStack(spacing: 8) {
                        ProgressView(value: Double(importer.listProgress.done), total: Double(max(importer.listProgress.total, 1))) {
                            Text("Reading the card… \(importer.listProgress.done) / \(importer.listProgress.total)")
                                .font(.callout)
                        }
                        .frame(maxWidth: 320)
                    }
                } else if let failure = importer.listFailure {
                    Text(verbatim: failure).font(.caption).foregroundStyle(.red).padding()
                } else if importer.visibleShots.isEmpty {
                    Text(importer.shots.isEmpty ? "No photos on the card." : "All photos on the card are already imported.")
                        .foregroundStyle(.secondary)
                } else {
                    grid
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(.background.secondary, in: RoundedRectangle(cornerRadius: 6))
        }
    }

    private var header: some View {
        HStack(spacing: 10) {
            if importer.hasList {
                Text("\(importer.checkedShots.count) of \(importer.selectableCount) photos selected, \(ByteCountFormatter.string(fromByteCount: importer.checkedBytes, countStyle: .file))")
                    .font(.callout)
                    .accessibilityIdentifier("importSelection")
            }
            Spacer()
            Button("Check All") { importer.checkAll() }
                .accessibilityIdentifier("importCheckAll")
            Button("Uncheck All") { importer.uncheckAll() }
                .accessibilityIdentifier("importUncheckAll")
            if importer.importedCount > 0 {
                Toggle("Hide Already Imported (\(importer.importedCount))", isOn: $importer.hideImported)
                    .toggleStyle(.checkbox)
                    .accessibilityIdentifier("importHideImported")
            }
        }
        .controlSize(.small)
        .disabled(!importer.hasList)
    }

    private var grid: some View {
        ScrollView {
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 112, maximum: 112), spacing: 10, alignment: .topLeading)],
                      alignment: .leading, spacing: 10) {
                ForEach(importer.visibleShots) { shot in
                    CardShotCell(shot: shot, importer: importer, checked: importer.isChecked(shot))
                        .onTapGesture { click(shot) }
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(10)
        }
        .accessibilityIdentifier("importGrid")
    }

    private func click(_ shot: CardShot) {
        guard !shot.imported else { return }
        let on = !importer.isChecked(shot)
        let visible = importer.visibleShots
        if NSEvent.modifierFlags.contains(.shift), let anchor,
           let a = visible.firstIndex(where: { $0.key == anchor }), let b = visible.firstIndex(where: { $0.key == shot.key }) {
            importer.setChecked(visible[min(a, b)...max(a, b)].filter { !$0.imported }.map(\.key), on)
        } else {
            importer.setChecked([shot.key], on)
        }
        anchor = shot.key
    }
}

private struct CardShotCell: View {
    let shot: CardShot
    let importer: CardImportModel
    let checked: Bool
    @State private var image: CGImage?

    var body: some View {
        VStack(spacing: 2) {
            ZStack {
                Rectangle().fill(.quaternary)
                if let image {
                    Image(decorative: image, scale: 1).resizable().scaledToFit()
                } else if !shot.isPhoto {
                    Image(systemName: "film").font(.title2).foregroundStyle(.secondary)
                }
            }
            .frame(width: 112, height: 112)
            .clipShape(RoundedRectangle(cornerRadius: 4))
            .overlay(alignment: .topLeading) {
                if !shot.imported {
                    Image(systemName: checked ? "checkmark.square.fill" : "square")
                        .font(.title3)
                        .symbolRenderingMode(.palette)
                        .foregroundStyle(checked ? Color.white : Color.secondary, checked ? Color.accentColor : Color.clear)
                        .background(Circle().fill(.black.opacity(0.25)).padding(2))
                        .padding(4)
                }
            }
            .overlay(alignment: .bottomTrailing) {
                if shot.imported {
                    Text("Imported")
                        .font(.caption2)
                        .padding(.horizontal, 5)
                        .padding(.vertical, 1)
                        .background(.thinMaterial, in: Capsule())
                        .padding(4)
                } else if shot.hasCompanion {
                    Text("+JPG")
                        .font(.caption2)
                        .padding(.horizontal, 4)
                        .background(.thinMaterial, in: Capsule())
                        .padding(4)
                }
            }
            .opacity(shot.imported ? 0.4 : 1)
            Text(shot.name).font(.caption).lineLimit(1).truncationMode(.middle).frame(width: 112)
            Text(captureText).font(.caption2).foregroundStyle(.secondary).lineLimit(1)
        }
        .contentShape(Rectangle())
        .task(id: shot.key) {
            image = importer.thumbnails.cached(shot)
            if image == nil { image = await importer.thumbnails.image(for: shot) }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(shot.name)
        .accessibilityValue(shot.imported ? "imported" : (checked ? "checked" : "unchecked"))
        .accessibilityAddTraits(.isButton)
        .accessibilityIdentifier("cardShot-\(shot.name)")
    }

    /// 'YYYY-MM-DD HH:MM'（撮影日時を読めず、更新日時で代用したものは ~ を付ける）
    private var captureText: String {
        let t = shot.captureTime
        return (shot.estimated ? "~" : "") + t.prefix(10) + " " + t.dropFirst(11).prefix(5)
    }
}
