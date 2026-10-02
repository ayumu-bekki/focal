import CFocal
import Foundation

/// カタログ（fc_catalog）。C API はスレッド安全なので、どのスレッドから呼んでもよい。
/// 呼び出しは SQLite への同期アクセスなので、大量の処理はメインスレッド以外で行うこと。
public final class Catalog: @unchecked Sendable {
    let handle: OpaquePointer

    public let url: URL

    public init(url: URL) throws {
        guard FocalCoreInfo.isCompatible else {
            throw FocalError(status: FC_ERR_UNSUPPORTED, message: "FocalCore API version mismatch")
        }
        var h: OpaquePointer?
        try url.path.withCString { try check(fc_catalog_open($0, &h)) }
        handle = h!
        self.url = url
    }

    deinit { fc_catalog_close(handle) }

    /// 積んである書き込み（編集の保存など）が終わるまで待つ。アプリの終了時に呼ぶ
    public func flush() throws { try check(fc_catalog_flush(handle)) }

    // MARK: ルート・フォルダ・タグ

    @discardableResult
    public func addRoot(_ dir: URL) throws -> Int64 {
        var id: Int64 = 0
        try dir.path.withCString { try check(fc_catalog_add_root(handle, $0, &id)) }
        return id
    }

    public func roots() throws -> [PhotoRoot] {
        var a: UnsafeMutablePointer<fc_root_array>?
        try check(fc_catalog_roots(handle, &a))
        defer { fc_root_array_free(a) }
        return UnsafeBufferPointer(start: a!.pointee.items, count: a!.pointee.count).map {
            PhotoRoot(id: $0.id, path: String(cString: $0.path), label: String(cString: $0.label))
        }
    }

    public func folders(rootID: Int64) throws -> [PhotoFolder] {
        var a: UnsafeMutablePointer<fc_folder_array>?
        try check(fc_catalog_folders(handle, rootID, &a))
        defer { fc_folder_array_free(a) }
        return UnsafeBufferPointer(start: a!.pointee.items, count: a!.pointee.count).map {
            PhotoFolder(id: $0.id, rootID: $0.root_id, parentID: $0.parent_id == 0 ? nil : $0.parent_id,
                        relativePath: String(cString: $0.rel_path), photoCount: $0.photo_count)
        }
    }

    public func tags() throws -> [PhotoTag] {
        var a: UnsafeMutablePointer<fc_tag_array>?
        try check(fc_catalog_tags(handle, &a))
        return Self.tags(from: a)
    }

    public func tags(ofPhoto id: Int64) throws -> [PhotoTag] {
        var a: UnsafeMutablePointer<fc_tag_array>?
        try check(fc_catalog_photo_tags(handle, id, &a))
        return Self.tags(from: a)
    }

    private static func tags(from a: UnsafeMutablePointer<fc_tag_array>?) -> [PhotoTag] {
        defer { fc_tag_array_free(a) }
        return UnsafeBufferPointer(start: a!.pointee.items, count: a!.pointee.count).map {
            PhotoTag(id: $0.id, parentID: $0.parent_id == 0 ? nil : $0.parent_id, name: String(cString: $0.name),
                     path: String(cString: $0.path), photoCount: $0.photo_count)
        }
    }

    @discardableResult
    public func ensureTag(_ path: String) throws -> Int64 {
        var id: Int64 = 0
        try path.withCString { try check(fc_catalog_ensure_tag(handle, $0, &id)) }
        return id
    }

    public func addTag(_ tagID: Int64, to photoIDs: [Int64]) throws {
        try photoIDs.withUnsafeBufferPointer { try check(fc_catalog_add_tag(handle, $0.baseAddress, $0.count, tagID)) }
    }

    public func removeTag(_ tagID: Int64, from photoIDs: [Int64]) throws {
        try photoIDs.withUnsafeBufferPointer {
            try check(fc_catalog_remove_tag(handle, $0.baseAddress, $0.count, tagID))
        }
    }

    // MARK: アルバム（v3.16）

    public func albums() throws -> [Album] {
        var a: UnsafeMutablePointer<fc_album_array>?
        try check(fc_catalog_albums(handle, &a))
        defer { fc_album_array_free(a) }
        return UnsafeBufferPointer(start: a!.pointee.items, count: a!.pointee.count).map {
            Album(id: $0.id, name: String(cString: $0.name), photoCount: $0.photo_count)
        }
    }

    /// 空や同じ名前のアルバムがあれば FocalError（invalidArgument）
    @discardableResult
    public func createAlbum(_ name: String) throws -> Int64 {
        var id: Int64 = 0
        try name.withCString { try check(fc_catalog_create_album(handle, $0, &id)) }
        return id
    }

    public func renameAlbum(_ id: Int64, to name: String) throws {
        try name.withCString { try check(fc_catalog_rename_album(handle, id, $0)) }
    }

    public func deleteAlbum(_ id: Int64) throws {
        try check(fc_catalog_delete_album(handle, id))
    }

    public func addToAlbum(_ albumID: Int64, photoIDs: [Int64]) throws {
        try photoIDs.withUnsafeBufferPointer {
            try check(fc_catalog_add_to_album(handle, albumID, $0.baseAddress, $0.count))
        }
    }

    public func removeFromAlbum(_ albumID: Int64, photoIDs: [Int64]) throws {
        try photoIDs.withUnsafeBufferPointer {
            try check(fc_catalog_remove_from_album(handle, albumID, $0.baseAddress, $0.count))
        }
    }

    // MARK: 写真

    public func count(_ filter: PhotoFilter = PhotoFilter()) throws -> Int64 {
        var n: Int64 = 0
        try filter.withCFilter { try check(fc_catalog_count(handle, $0, &n)) }
        return n
    }

    public func photos(_ filter: PhotoFilter = PhotoFilter(), offset: Int64 = 0, limit: Int64 = -1) throws -> [Photo] {
        var a: UnsafeMutablePointer<fc_photo_array>?
        try filter.withCFilter { try check(fc_catalog_query(handle, $0, offset, limit, &a)) }
        return Self.photos(from: a)
    }

    /// 絞り込み結果の id だけ（10 万件のグリッド用）
    public func photoIDs(_ filter: PhotoFilter = PhotoFilter()) throws -> [Int64] {
        var a: UnsafeMutablePointer<fc_id_array>?
        try filter.withCFilter { try check(fc_catalog_query_ids(handle, $0, &a)) }
        defer { fc_id_array_free(a) }
        return Array(UnsafeBufferPointer(start: a!.pointee.items, count: a!.pointee.count))
    }

    /// 指定した id の写真を ids の順で返す（存在しない id は飛ばす）
    public func photos(ids: [Int64]) throws -> [Photo] {
        var a: UnsafeMutablePointer<fc_photo_array>?
        try ids.withUnsafeBufferPointer { try check(fc_catalog_photos_by_ids(handle, $0.baseAddress, $0.count, &a)) }
        return Self.photos(from: a)
    }

    private static func photos(from a: UnsafeMutablePointer<fc_photo_array>?) -> [Photo] {
        defer { fc_photo_array_free(a) }
        return UnsafeBufferPointer(start: a!.pointee.items, count: a!.pointee.count).map(Photo.init)
    }

    public func setRating(_ rating: Int, for ids: [Int64]) throws {
        try ids.withUnsafeBufferPointer {
            try check(fc_catalog_set_rating(handle, $0.baseAddress, $0.count, Int32(rating)))
        }
    }

    public func setFlag(_ flag: PhotoFlag, for ids: [Int64]) throws {
        try ids.withUnsafeBufferPointer {
            try check(fc_catalog_set_flag(handle, $0.baseAddress, $0.count, flag.rawValue))
        }
    }

    // MARK: 書き出し（5.8 章）

    public struct ExportOptions: Sendable {
        public enum Format: Int32, Sendable { case jpeg = 0, tiff16 = 1 }
        public var format: Format = .jpeg
        public var quality = 92
        /// 0 なら原寸
        public var longEdge = 0
        public var destination: URL
        public init(destination: URL) { self.destination = destination }
    }

    public enum ExportEvent: Sendable {
        /// 1 枚終わった。成功なら output、失敗なら error
        case item(done: Int, total: Int, photoID: Int64, output: URL?, error: String?)
        /// cancelled: キャンセルで残りを書き出さなかった
        case finished(cancelled: Bool)
    }

    /// 写真を順に書き出す。ストリームを途中で捨てる（Task をキャンセルする）と書き出しもキャンセルする
    public func export(_ ids: [Int64], options: ExportOptions) -> AsyncThrowingStream<ExportEvent, Error> {
        AsyncThrowingStream { continuation in
            let box = ExportBox(continuation: continuation, catalog: self)
            let user = Unmanaged.passRetained(box).toOpaque()
            var opt = fc_export_options()
            opt.format = options.format.rawValue
            opt.quality = Int32(options.quality)
            opt.long_edge = Int32(options.longEdge)
            var task: OpaquePointer?
            let status = options.destination.path.withCString { dir in
                opt.dest_dir = dir
                return ids.withUnsafeBufferPointer {
                    fc_export_start(handle, $0.baseAddress, $0.count, &opt, exportProgress, exportDone, user, &task)
                }
            }
            if status != FC_OK {
                Unmanaged<ExportBox>.fromOpaque(user).release()
                continuation.finish(throwing: FocalError(status: status))
                return
            }
            box.setTask(task!)
            continuation.onTermination = { @Sendable _ in box.cancel() }
        }
    }

    // MARK: スキャン

    public enum ScanEvent: Sendable {
        case progress(done: Int, total: Int)
        case finished(ScanStats)
    }

    /// ルート以下を core のスレッドで走査する。ストリームを途中で捨てる（Task をキャンセルする）とスキャンもキャンセルする。
    public func scan(rootID: Int64, thumbnailCache: URL?) -> AsyncThrowingStream<ScanEvent, Error> {
        AsyncThrowingStream { continuation in
            let box = ScanBox(continuation: continuation, catalog: self)
            let user = Unmanaged.passRetained(box).toOpaque()
            var task: OpaquePointer?
            let status = withOptionalCString(thumbnailCache?.path) { cache in
                fc_catalog_scan_async(handle, rootID, cache, scanProgress, scanDone, user, &task)
            }
            if status != FC_OK {
                Unmanaged<ScanBox>.fromOpaque(user).release()
                continuation.finish(throwing: FocalError(status: status))
                return
            }
            box.setTask(task!)
            continuation.onTermination = { @Sendable _ in box.cancel() }
        }
    }
}

/// スキャン中の状態。完了コールバックで解放する
private final class ScanBox: @unchecked Sendable {
    let continuation: AsyncThrowingStream<Catalog.ScanEvent, Error>.Continuation
    let catalog: Catalog  // スキャン中はカタログを閉じない
    private let lock = NSLock()
    private var task: OpaquePointer?
    private var finished = false

    init(continuation: AsyncThrowingStream<Catalog.ScanEvent, Error>.Continuation, catalog: Catalog) {
        self.continuation = continuation
        self.catalog = catalog
    }

    func setTask(_ t: OpaquePointer) {
        lock.lock()
        task = t
        let alreadyFinished = finished
        lock.unlock()
        if alreadyFinished { releaseTask() }
    }

    func cancel() {
        lock.lock()
        if let task, !finished { fc_task_cancel(task) }
        lock.unlock()
    }

    /// 完了コールバックから呼ぶ。fc_task_release はスレッドの終了を待つので、別のスレッドで呼ぶ
    func finish() {
        lock.lock()
        finished = true
        let hasTask = task != nil
        lock.unlock()
        if hasTask { releaseTask() }
    }

    private func releaseTask() {
        lock.lock()
        let t = task
        task = nil
        lock.unlock()
        guard let t else { return }
        let handle = UInt(bitPattern: t)
        DispatchQueue.global(qos: .utility).async {
            fc_task_release(OpaquePointer(bitPattern: handle))
        }
    }
}

private func scanProgress(_ user: UnsafeMutableRawPointer?, _ done: Int32, _ total: Int32) {
    let box = Unmanaged<ScanBox>.fromOpaque(user!).takeUnretainedValue()
    box.continuation.yield(.progress(done: Int(done), total: Int(total)))
}

private func scanDone(_ user: UnsafeMutableRawPointer?, _ status: fc_status, _ stats: UnsafePointer<fc_scan_stats>?,
                      _ message: UnsafePointer<CChar>?) {
    let box = Unmanaged<ScanBox>.fromOpaque(user!).takeRetainedValue()
    if status == FC_OK {
        box.continuation.yield(.finished(ScanStats(stats!.pointee)))
        box.continuation.finish()
    } else {
        box.continuation.finish(throwing: FocalError(status: status, message: String(optionalCString: message) ?? ""))
    }
    box.finish()
}

/// 書き出し中の状態。完了コールバックで解放する
private final class ExportBox: @unchecked Sendable {
    let continuation: AsyncThrowingStream<Catalog.ExportEvent, Error>.Continuation
    let catalog: Catalog
    private let lock = NSLock()
    private var task: OpaquePointer?
    private var finished = false

    init(continuation: AsyncThrowingStream<Catalog.ExportEvent, Error>.Continuation, catalog: Catalog) {
        self.continuation = continuation
        self.catalog = catalog
    }

    func setTask(_ t: OpaquePointer) {
        lock.lock()
        task = t
        let done = finished
        lock.unlock()
        if done { releaseTask() }
    }

    func cancel() {
        lock.lock()
        if let task, !finished { fc_task_cancel(task) }
        lock.unlock()
    }

    func finish() {
        lock.lock()
        finished = true
        let has = task != nil
        lock.unlock()
        if has { releaseTask() }
    }

    private func releaseTask() {
        lock.lock()
        let t = task
        task = nil
        lock.unlock()
        guard let t else { return }
        let handle = UInt(bitPattern: t)
        DispatchQueue.global(qos: .utility).async { fc_task_release(OpaquePointer(bitPattern: handle)) }
    }
}

private func exportProgress(_ user: UnsafeMutableRawPointer?, _ done: Int32, _ total: Int32, _ id: Int64, _ ok: Int32,
                            _ text: UnsafePointer<CChar>?) {
    let box = Unmanaged<ExportBox>.fromOpaque(user!).takeUnretainedValue()
    let s = String(optionalCString: text) ?? ""
    box.continuation.yield(.item(done: Int(done), total: Int(total), photoID: id,
                                 output: ok != 0 ? URL(fileURLWithPath: s) : nil, error: ok != 0 ? nil : s))
}

private func exportDone(_ user: UnsafeMutableRawPointer?, _ status: fc_status) {
    let box = Unmanaged<ExportBox>.fromOpaque(user!).takeRetainedValue()
    box.continuation.yield(.finished(cancelled: status != FC_OK))
    box.continuation.finish()
    box.finish()
}
