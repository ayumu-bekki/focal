import FocalCore
import SwiftUI

/// SwiftUI の LazyVGrid によるグリッド（9.1 章: NSCollectionView 版と 10 万件で比べるための実装）
struct PhotoLazyGrid: View {
    @Bindable var model: LibraryModel

    var body: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVGrid(columns: [GridItem(.adaptive(minimum: model.thumbnailSize, maximum: model.thumbnailSize), spacing: 8)],
                          spacing: 8) {
                    ForEach(Array(model.photoIDs.enumerated()), id: \.element) { index, id in
                        LazyGridCell(model: model, index: index, photoID: id)
                            .id(id)
                            .onTapGesture(count: 2) { model.select(index: index); model.mode = .viewer }
                            .onTapGesture { model.select(index: index, extend: NSEvent.modifierFlags.contains(.command)) }
                    }
                }
                .padding(8)
                .id(model.listGeneration)
            }
            .accessibilityIdentifier("photoGrid")
            .onChange(of: model.currentIndex) { _, i in
                if let i, model.photoIDs.indices.contains(i) { proxy.scrollTo(model.photoIDs[i]) }
            }
        }
    }
}

private struct LazyGridCell: View {
    let model: LibraryModel
    let index: Int
    let photoID: Int64
    @State private var image: CGImage?

    var body: some View {
        let photo = model.photo(at: index)
        let selected = model.selection.contains(photoID)
        VStack(spacing: 2) {
            ZStack {
                Rectangle().fill(.quaternary)
                if let image {
                    Image(decorative: image, scale: 1).resizable().scaledToFit()
                }
            }
            .frame(width: model.thumbnailSize, height: model.thumbnailSize - 2)
            HStack {
                Text(photo?.fileName ?? "").lineLimit(1).truncationMode(.middle).foregroundStyle(.secondary)
                Spacer(minLength: 2)
                Text(String(repeating: "★", count: photo?.rating ?? 0) + (photo?.flag == .picked ? " ⚑" : photo?.flag == .rejected ? " ✕" : ""))
            }
            .font(.system(size: 11))
        }
        .padding(2)
        .background(selected ? Color.accentColor.opacity(0.35) : .clear, in: RoundedRectangle(cornerRadius: 4))
        .opacity(photo?.flag == .rejected ? 0.5 : 1)
        .task(id: photoID) {
            image = model.thumbnails.cached(photoID)
            if image == nil { image = await model.thumbnails.image(for: photoID) }
        }
    }
}
