import CoreServices
import Foundation

/// 表示中のフォルダの変更を見る（FSEvents、v3.19）。外でファイルが増減・移動されたら、少し待ってから onChange を呼ぶ。
/// 監視は 1 か所だけ（全体の常時監視はしない）。隠しファイル（.DS_Store など）だけの変化は無視する
@MainActor
final class FolderWatcher {
    private var stream: FSEventStreamRef?
    private var path: String?
    private var onChange: (() -> Void)?
    private var debounce: Task<Void, Never>?

    func watch(path: String, onChange: @escaping () -> Void) {
        self.onChange = onChange
        if self.path == path, stream != nil { return }
        stop()
        self.path = path
        self.onChange = onChange

        var context = FSEventStreamContext(version: 0, info: Unmanaged.passUnretained(self).toOpaque(), retain: nil,
                                           release: nil, copyDescription: nil)
        let callback: FSEventStreamCallback = { _, info, count, eventPaths, _, _ in
            guard let info else { return }
            let paths = (unsafeBitCast(eventPaths, to: NSArray.self) as? [String]) ?? []
            let visible = paths.prefix(count).contains { !URL(fileURLWithPath: $0).lastPathComponent.hasPrefix(".") }
            guard visible else { return }
            // ストリームはメインキューで動かしているので、ここはメインスレッド
            MainActor.assumeIsolated {
                Unmanaged<FolderWatcher>.fromOpaque(info).takeUnretainedValue().eventArrived()
            }
        }
        let flags = FSEventStreamCreateFlags(kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagUseCFTypes)
        guard let s = FSEventStreamCreate(nil, callback, &context, [path] as CFArray,
                                          FSEventStreamEventId(kFSEventStreamEventIdSinceNow), 1.0, flags) else { return }
        FSEventStreamSetDispatchQueue(s, .main)
        FSEventStreamStart(s)
        stream = s
    }

    func stop() {
        debounce?.cancel()
        debounce = nil
        if let s = stream {
            FSEventStreamStop(s)
            FSEventStreamInvalidate(s)
            FSEventStreamRelease(s)
        }
        stream = nil
        path = nil
        onChange = nil
    }

    private func eventArrived() {
        debounce?.cancel()
        debounce = Task { [weak self] in
            try? await Task.sleep(for: .seconds(1.5))
            guard !Task.isCancelled else { return }
            self?.onChange?()
        }
    }
}
