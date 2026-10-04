import AppKit
import FocalCore
import SwiftUI

/// NSCollectionView によるグリッド（9.1 章、10 万件でも滑らかにするための第一候補）。
/// 見えているセルだけがサムネイルを要求し、画面外に出たら要求をキャンセルする（10 章）。
/// 現像画面の下のフィルムストリップ（9.5 章）も同じ部品を横向きにして使う
struct PhotoCollectionView: NSViewRepresentable {
    enum Style { case grid, filmstrip }

    let model: LibraryModel
    var style: Style = .grid

    /// フィルムストリップの高さ（pt）

    func makeCoordinator() -> Coordinator { Coordinator(model: model, style: style) }

    func makeNSView(context: Context) -> NSScrollView {
        let layout = NSCollectionViewFlowLayout()
        layout.minimumInteritemSpacing = style == .grid ? 8 : 4
        layout.minimumLineSpacing = style == .grid ? 8 : 4
        layout.sectionInset = style == .grid ? NSEdgeInsets(top: 8, left: 8, bottom: 8, right: 8)
                                              : NSEdgeInsets(top: 6, left: 6, bottom: 6, right: 6)
        if style == .filmstrip { layout.scrollDirection = .horizontal }

        let cv = KeyCollectionView()
        cv.collectionViewLayout = layout
        cv.isSelectable = true
        cv.allowsMultipleSelection = style == .grid
        cv.backgroundColors = [.controlBackgroundColor]
        cv.register(PhotoGridItem.self, forItemWithIdentifier: PhotoGridItem.identifier)
        // 写真をサイドバーのアルバムへドラッグできる（v3.16）
        cv.setDraggingSourceOperationMask(.copy, forLocal: true)
        cv.dataSource = context.coordinator
        cv.delegate = context.coordinator
        cv.model = model
        // フィルムストリップはキー操作を受けない（現像画面の矢印キーは KeyMonitor が受ける）
        cv.takesFocus = style == .grid
        cv.setAccessibilityIdentifier(style == .grid ? "photoGrid" : "filmstrip")
        context.coordinator.collectionView = cv

        let scroll = NSScrollView()
        scroll.documentView = cv
        scroll.hasVerticalScroller = style == .grid
        scroll.hasHorizontalScroller = style == .filmstrip
        scroll.autohidesScrollers = true
        scroll.drawsBackground = false
        return scroll
    }

    func updateNSView(_ scroll: NSScrollView, context: Context) {
        context.coordinator.sync(with: model)
    }

    @MainActor
    final class Coordinator: NSObject, NSCollectionViewDataSource, NSCollectionViewDelegateFlowLayout {
        let model: LibraryModel
        let style: Style
        weak var collectionView: NSCollectionView?
        private var generation = -1
        private var changedGeneration = 0
        private var thumbnailGeneration = 0
        private var itemSize: Double = 0
        private var filmstripHeight: Double = 0
        private var syncingSelection = false

        init(model: LibraryModel, style: Style) {
            self.model = model
            self.style = style
        }

        func sync(with model: LibraryModel) {
            guard let cv = collectionView else { return }
            if generation != model.listGeneration {
                generation = model.listGeneration
                cv.reloadData()
            } else if changedGeneration != model.changedPhotos.generation {
                // ★・フラグだけ変わった: 見えているセルの表示だけ更新する
                for case let item as PhotoGridItem in cv.visibleItems() where model.changedPhotos.ids.contains(item.photoID) {
                    if let i = cv.indexPath(for: item)?.item { item.update(photo: model.photo(at: i)) }
                }
            }
            changedGeneration = model.changedPhotos.generation
            if thumbnailGeneration != model.reloadedThumbnails.generation {
                thumbnailGeneration = model.reloadedThumbnails.generation
                for case let item as PhotoGridItem in cv.visibleItems()
                where model.reloadedThumbnails.ids.contains(item.photoID) {
                    item.reloadImage()
                }
            }
            if itemSize != model.thumbnailSize {
                itemSize = model.thumbnailSize
                cv.collectionViewLayout?.invalidateLayout()
            }
            if style == .filmstrip, filmstripHeight != model.filmstripHeight {  // フィルムストリップの高さを変えた
                filmstripHeight = model.filmstripHeight
                cv.collectionViewLayout?.invalidateLayout()
            }
            // 選択（キー操作で動いたとき）をグリッドに反映してスクロールする
            if let i = model.currentIndex {
                let ip = IndexPath(item: i, section: 0)
                if !cv.selectionIndexPaths.contains(ip) || cv.selectionIndexPaths.count != model.selection.count {
                    syncingSelection = true
                    let paths = Set(model.selection.compactMap { id in
                        model.photoIDs.firstIndex(of: id).map { IndexPath(item: $0, section: 0) }
                    })
                    cv.selectionIndexPaths = paths.isEmpty ? [ip] : paths
                    // フィルムストリップは選択中の写真を中央に
                    cv.scrollToItems(at: [ip], scrollPosition: style == .grid ? .nearestHorizontalEdge : .centeredHorizontally)
                    syncingSelection = false
                }
            }
        }

        func collectionView(_ cv: NSCollectionView, numberOfItemsInSection section: Int) -> Int { model.photoIDs.count }

        func collectionView(_ cv: NSCollectionView, itemForRepresentedObjectAt indexPath: IndexPath) -> NSCollectionViewItem {
            let item = cv.makeItem(withIdentifier: PhotoGridItem.identifier, for: indexPath) as! PhotoGridItem
            item.loader = model.thumbnails
            item.compact = style == .filmstrip
            item.update(photo: model.photo(at: indexPath.item))
            return item
        }

        func collectionView(_ cv: NSCollectionView, willDisplay item: NSCollectionViewItem,
                            forRepresentedObjectAt indexPath: IndexPath) {
            (item as? PhotoGridItem)?.startLoading()
        }

        func collectionView(_ cv: NSCollectionView, didEndDisplaying item: NSCollectionViewItem,
                            forRepresentedObjectAt indexPath: IndexPath) {
            (item as? PhotoGridItem)?.cancelLoading()
        }

        func collectionView(_ cv: NSCollectionView, layout: NSCollectionViewLayout,
                            sizeForItemAt indexPath: IndexPath) -> NSSize {
            if style == .filmstrip {
                let h = model.filmstripHeight - 12
                return NSSize(width: (h * 1.3).rounded(), height: h)
            }
            return NSSize(width: model.thumbnailSize, height: model.thumbnailSize + 22)
        }

        func collectionView(_ cv: NSCollectionView, canDragItemsAt indexPaths: Set<IndexPath>, with event: NSEvent) -> Bool {
            true
        }

        func collectionView(_ cv: NSCollectionView, pasteboardWriterForItemAt indexPath: IndexPath) -> NSPasteboardWriting? {
            guard model.photoIDs.indices.contains(indexPath.item) else { return nil }
            return PhotoDrag.string(for: model.photoIDs[indexPath.item]) as NSString
        }

        func collectionView(_ cv: NSCollectionView, didSelectItemsAt indexPaths: Set<IndexPath>) { syncSelection(cv) }
        func collectionView(_ cv: NSCollectionView, didDeselectItemsAt indexPaths: Set<IndexPath>) { syncSelection(cv) }

        private func syncSelection(_ cv: NSCollectionView) {
            guard !syncingSelection else { return }
            let indexes = cv.selectionIndexPaths.map(\.item).sorted()
            model.selection = Set(indexes.compactMap { model.photoIDs.indices.contains($0) ? model.photoIDs[$0] : nil })
            if let last = indexes.last, model.currentIndex.map({ !indexes.contains($0) }) ?? true {
                model.currentIndex = last
            }
            if indexes.count == 1 { model.selectionAnchor = indexes[0] }  // キー操作などで 1 枚になったとき
        }
    }
}

/// グリッドのコレクションビュー。単キーの操作は KeyMonitor が受け、矢印キーは NSCollectionView の標準動作に任せる
final class KeyCollectionView: NSCollectionView {
    weak var model: LibraryModel?
    /// 画面に出たときにキー操作を受ける（グリッド）。フィルムストリップは受けない
    var takesFocus = true

    override var acceptsFirstResponder: Bool { takesFocus }

    // ビューアから戻ったときにすぐキー操作を受けられるようにする
    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        guard takesFocus else { return }
        DispatchQueue.main.async { [weak self] in
            guard let self, let window = self.window else { return }
            window.makeFirstResponder(self)
        }
    }

    /// 一覧の選択（Photomator・「写真」と同じ）: ふつうのクリックは 1 枚、⌘クリックは 1 枚ずつ追加・解除、
    /// ⇧クリックは起点からその写真までの範囲（⇧⌘ならいまの選択に範囲を足す）。フィルムストリップは 1 枚だけ
    override func mouseDown(with event: NSEvent) {
        let mods = event.modifierFlags.intersection([.shift, .command, .option, .control])
        if takesFocus, let model, mods.contains(.shift), !mods.contains(.option), !mods.contains(.control),
           let ip = indexPathForItem(at: convert(event.locationInWindow, from: nil)) {
            model.selectRange(to: ip.item, additive: mods.contains(.command))
            window?.makeFirstResponder(self)
            return  // 標準の動作（1 枚ずつ追加）には渡さない
        }
        super.mouseDown(with: event)
        if takesFocus, let model, let ip = indexPathForItem(at: convert(event.locationInWindow, from: nil)) {
            // ⌘クリックで選んだ写真も、次の ⇧クリックの起点にする。選びを外したときは、起点を変えない
            // （⌘クリックの解除は、mouseDown が戻ったあとに反映されるので、次の周回で確かめる）
            DispatchQueue.main.async { [weak self, weak model] in
                if self?.selectionIndexPaths.contains(ip) == true { model?.selectionAnchor = ip.item }
            }
        }
        if event.clickCount == 2, takesFocus { model?.mode = .viewer }
    }
}

/// グリッドとビューアで共通の単キー操作。処理したら true
@MainActor
func handleLibraryKey(_ event: NSEvent, model: LibraryModel) -> Bool {
    let mods = event.modifierFlags.intersection([.command, .control, .option])
    guard mods.isEmpty, let ch = event.charactersIgnoringModifiers?.lowercased(), ch.count == 1 else { return false }
    switch ch {
    case "0", "1", "2", "3", "4", "5": model.setRating(Int(ch)!)
    case "p": model.setFlag(.picked)
    case "x": model.setFlag(.rejected)
    case "u": model.setFlag(.none)
    case "v": model.toggleMode()
    default: return false
    }
    return true
}

/// グリッドの 1 マス: サムネイル + ★/フラグ
final class PhotoGridItem: NSCollectionViewItem {
    static let identifier = NSUserInterfaceItemIdentifier("PhotoGridItem")

    var loader: ThumbnailLoader?
    /// フィルムストリップ: サムネイルだけ（ファイル名を出さず、★・フラグは右下に重ねる）
    var compact = false {
        didSet {
            caption.isHidden = compact
            view.needsLayout = true
        }
    }
    private(set) var photoID: Int64 = 0
    private var loadTask: Task<Void, Never>?
    private let imageLayer = CALayer()
    private let caption = NSTextField(labelWithString: "")
    private let badge = NSTextField(labelWithString: "")

    override func loadView() {
        let v = NSView()
        v.wantsLayer = true
        v.layer?.cornerRadius = 4
        imageLayer.contentsGravity = .resizeAspect
        imageLayer.backgroundColor = NSColor.quaternaryLabelColor.cgColor
        v.layer?.addSublayer(imageLayer)

        caption.font = .systemFont(ofSize: 11)
        caption.textColor = .secondaryLabelColor
        caption.lineBreakMode = .byTruncatingMiddle
        badge.font = .systemFont(ofSize: 11)
        badge.alignment = .right
        for f in [caption, badge] {
            f.translatesAutoresizingMaskIntoConstraints = false
            v.addSubview(f)
        }
        NSLayoutConstraint.activate([
            caption.leadingAnchor.constraint(equalTo: v.leadingAnchor, constant: 2),
            caption.bottomAnchor.constraint(equalTo: v.bottomAnchor, constant: -2),
            badge.trailingAnchor.constraint(equalTo: v.trailingAnchor, constant: -2),
            badge.bottomAnchor.constraint(equalTo: v.bottomAnchor, constant: -2),
            caption.trailingAnchor.constraint(lessThanOrEqualTo: badge.leadingAnchor, constant: -4),
        ])
        view = v
    }

    override func viewDidLayout() {
        super.viewDidLayout()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        let captionHeight: CGFloat = compact ? 0 : 20
        imageLayer.frame = NSRect(x: 0, y: captionHeight, width: view.bounds.width,
                                  height: view.bounds.height - captionHeight)
        CATransaction.commit()
    }

    override var isSelected: Bool {
        didSet {
            view.layer?.backgroundColor = isSelected ? NSColor.selectedContentBackgroundColor.withAlphaComponent(0.35).cgColor : nil
        }
    }

    func update(photo: Photo?) {
        let newID = photo?.id ?? 0
        if newID != photoID {
            cancelLoading()
            photoID = newID
            imageLayer.contents = loader?.cached(newID)
        }
        caption.stringValue = photo?.fileName ?? ""
        var b = String(repeating: "★", count: photo?.rating ?? 0)
        switch photo?.flag {
        case .picked: b += " ⚑"
        case .rejected: b += " ✕"
        default: break
        }
        if photo?.status == .missing { b += " ?" }
        badge.stringValue = b
        badge.textColor = photo?.flag == .rejected ? .systemRed : .labelColor
        view.alphaValue = photo?.flag == .rejected ? 0.5 : 1.0
        view.setAccessibilityLabel(photo?.fileName)
        view.setAccessibilityIdentifier("photoCell")
    }

    func startLoading() {
        guard let loader, photoID != 0, imageLayer.contents == nil, loadTask == nil else { return }
        let id = photoID
        loadTask = Task { [weak self] in
            let img = await loader.image(for: id)
            guard let self, !Task.isCancelled, self.photoID == id else { return }
            self.imageLayer.contents = img
            self.loadTask = nil
        }
    }

    /// 編集後のサムネイルに差し替える（今の画像は新しい画像が届くまで残す）
    func reloadImage() {
        cancelLoading()
        guard let loader, photoID != 0 else { return }
        let id = photoID
        loadTask = Task { [weak self] in
            let img = await loader.image(for: id)
            guard let self, !Task.isCancelled, self.photoID == id, let img else { return }
            self.imageLayer.contents = img
            self.loadTask = nil
        }
    }

    func cancelLoading() {
        loadTask?.cancel()
        loadTask = nil
    }

    override func prepareForReuse() {
        super.prepareForReuse()
        cancelLoading()
        imageLayer.contents = nil
        photoID = 0
    }
}
