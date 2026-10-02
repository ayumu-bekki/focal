import AppKit

/// 単キーの操作（9.2 章）をウィンドウ単位で受ける。
/// テキスト入力中（タグ名の入力など）は横取りしない。グリッドの矢印キーは NSCollectionView に任せる。
/// カタログを切り替えたとき（ContentView を作り直したとき）に外す。
@MainActor
final class KeyMonitor {
    private var token: Any?

    func uninstall() {
        if let token { NSEvent.removeMonitor(token) }
        token = nil
    }

    func install(model: LibraryModel) {
        guard token == nil else { return }
        token = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak model] event in
            guard let model else { return event }
            if NSApp.keyWindow?.firstResponder is NSText { return event }  // 入力中
            if NSApp.keyWindow?.isSheet == true || NSApp.keyWindow?.attachedSheet != nil { return event }  // シート
            if model.mode == .viewer, event.modifierFlags.intersection([.command, .control, .option]).isEmpty {
                let ch = event.charactersIgnoringModifiers?.lowercased()
                if ch == "c" {  // クロップモードの開始・確定（9.2 章）
                    model.develop.toggleCropMode()
                    return nil
                }
                if ch == "[" || ch == "]" {  // 90° 回転
                    model.develop.rotate(by: ch == "]" ? 1 : -1)
                    return nil
                }
                if model.develop.cropMode {
                    switch event.keyCode {
                    case 53: model.develop.cancelCrop(); return nil      // Esc
                    case 36, 76: model.develop.commitCrop(); return nil  // Return / Enter
                    default: break
                    }
                }
                if event.charactersIgnoringModifiers?.lowercased() == "z" {
                    // フィット ⇄ 100%（ポインタの位置を中心に、9.2 章）
                    let view = NSApp.keyWindow?.contentView.flatMap(Self.developView(in:))
                    model.develop.toggleZoom(at: view?.pointerInOutput())
                    return nil
                }
                switch event.keyCode {
                case 123, 126: model.move(by: -1); return nil  // ← ↑
                case 124, 125: model.move(by: 1); return nil   // → ↓
                case 53: model.mode = .grid; return nil         // Esc
                default: break
                }
            }
            return handleLibraryKey(event, model: model) ? nil : event
        }
    }

    private static func developView(in view: NSView) -> DevelopNSView? {
        if let d = view as? DevelopNSView { return d }
        for sub in view.subviews { if let d = developView(in: sub) { return d } }
        return nil
    }
}
