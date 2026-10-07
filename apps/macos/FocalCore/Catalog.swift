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
            PhotoRoot(id: $0.id, path: String(cString: $0.path), label: String(cString: $0.label),
                      volumeID: String(cString: $0.volume_id), volumeName: String(cString: $0.volume_name),
                      volumeRelativePath: String(cString: $0.volume_rel_path), isOnline: $0.online != 0)
        }
    }

    /// 外付けドライブのマウントポイントが変わっていたら、ルートの場所を合わせる。変えたルートの数を返す
    @discardableResult
    public func refreshVolumes() throws -> Int {
        var n: Int32 = 0
        try check(fc_catalog_refresh_volumes(handle, &n))
        return Int(n)
    }

    /// ルートをカタログから外す。写真の情報（★・タグ・編集など）も消える。ディスク上のファイルは消さない
    public func removeRoot(_ rootID: Int64) throws {
        try check(fc_catalog_remove_root(handle, rootID))
    }

    /// ルートの詳しい情報。ボリュームの容量を取るので、メインスレッド以外で呼ぶこと（応答しない共有は 1.5 秒で諦める）
    public func rootDetails(rootID: Int64) throws -> RootDetails {
        var p: UnsafeMutablePointer<fc_root_details>?
        try check(fc_catalog_root_details(handle, rootID, &p))
        defer { fc_root_details_free(p) }
        let d = p!.pointee
        func s(_ c: UnsafePointer<CChar>?) -> String { c.map { String(cString: $0) } ?? "" }
        return RootDetails(
            id: d.id, path: s(d.path), label: s(d.label), volumeID: s(d.volume_id), volumeName: s(d.volume_name),
            mountPoint: s(d.mount_point), fileSystem: s(d.fs_type), isOnline: d.online != 0,
            kind: RootDetails.Kind(rawValue: d.kind) ?? .unknown,
            totalBytes: d.total_bytes >= 0 ? d.total_bytes : nil, freeBytes: d.free_bytes >= 0 ? d.free_bytes : nil,
            photos: d.photos, folders: d.folders, missing: d.missing, editedPhotos: d.edited_photos,
            totalFileBytes: d.total_file_bytes,
            captureFrom: s(d.capture_from).isEmpty ? nil : s(d.capture_from),
            captureTo: s(d.capture_to).isEmpty ? nil : s(d.capture_to))
    }

    /// dir を含む登録済みのルートと、その中のフォルダの id。なければ nil（スキャン前は、フォルダの行がなくて nil のことがある）
    public func folderLocation(forPath dir: URL) -> (rootID: Int64, folderID: Int64)? {
        var r: Int64 = 0, f: Int64 = 0
        let status = dir.path.withCString { fc_catalog_folder_for_path(handle, $0, &r, &f) }
        return status == FC_OK ? (r, f) : nil
    }

    /// ルートの場所を付け替える。写真の行（現像・★・タグ）はそのまま。付け替えたあと、そのルートをスキャンする
    public func relocateRoot(_ rootID: Int64, to dir: URL) throws {
        try dir.path.withCString { try check(fc_catalog_relocate_root(handle, rootID, $0)) }
    }

    /// 重なっているルート（別のルートの中にあるルート）の数。以前の二重登録の跡
    public func nestedRootCount() throws -> Int {
        var n: Int32 = 0
        try check(fc_catalog_nested_root_count(handle, &n))
        return Int(n)
    }

    /// 重なっているルートをすべて統合する（現像・★・タグは写真ごと残る。変更の前に、バックアップを作る）。統合した数
    @discardableResult
    public func mergeNestedRoots() throws -> Int {
        var n: Int32 = 0
        try check(fc_catalog_merge_nested_roots(handle, &n))
        return Int(n)
    }

    /// カタログの情報（大きさ・写真の数・保管した情報・バックアップ。v3.23）
    public func info() throws -> CatalogInfo {
        var i = fc_catalog_info()
        try check(fc_catalog_get_info(handle, &i))
        return CatalogInfo(i)
    }

    /// カタログから外したときに保管して、まだ写真につながっていない情報の数（v3.23）
    public func detachedSummary() throws -> DetachedSummary {
        var d = fc_detached_summary()
        try check(fc_catalog_detached_summary(handle, &d))
        return DetachedSummary(items: Int(d.items), edits: Int(d.edits))
    }

    /// カタログの最適化: 保管した情報を消して、ファイルを詰める。元に戻せないので、先にバックアップを作る。
    /// 時間がかかることがあるので、メインスレッド以外で呼ぶこと
    public func optimize() throws -> OptimizeResult {
        var r = fc_optimize_result()
        try check(fc_catalog_optimize(handle, &r))
        return OptimizeResult(removedItems: Int(r.removed_items), bytesBefore: r.bytes_before, bytesAfter: r.bytes_after)
    }

    public func setRootLabel(_ label: String, rootID: Int64) throws {
        try label.withCString { try check(fc_catalog_set_root_label(handle, rootID, $0)) }
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
            Album(id: $0.id, name: String(cString: $0.name), photoCount: $0.photo_count,
                  parentID: $0.parent_id == 0 ? nil : $0.parent_id, kind: Album.Kind(rawValue: $0.kind) ?? .album,
                  coverPhotoID: $0.cover_photo_id == 0 ? nil : $0.cover_photo_id)
        }
    }

    /// 空や、同じ親の下に同じ名前のアルバムがあれば FocalError（invalidArgument）。parentID はフォルダの id
    @discardableResult
    public func createAlbum(_ name: String, parentID: Int64? = nil) throws -> Int64 {
        var id: Int64 = 0
        try name.withCString { try check(fc_catalog_create_album(handle, $0, parentID ?? 0, &id)) }
        return id
    }

    @discardableResult
    public func createAlbumFolder(_ name: String, parentID: Int64? = nil) throws -> Int64 {
        var id: Int64 = 0
        try name.withCString { try check(fc_catalog_create_album_folder(handle, $0, parentID ?? 0, &id)) }
        return id
    }

    /// queryJSON は design.md 7.2 章のスマートアルバムの条件。不正なら FocalError（invalidArgument）
    @discardableResult
    public func createSmartAlbum(_ name: String, queryJSON: String, parentID: Int64? = nil) throws -> Int64 {
        var id: Int64 = 0
        try name.withCString { n in
            try queryJSON.withCString { try check(fc_catalog_create_smart_album(handle, n, $0, parentID ?? 0, &id)) }
        }
        return id
    }

    public func setSmartQuery(_ id: Int64, queryJSON: String) throws {
        try queryJSON.withCString { try check(fc_catalog_set_smart_query(handle, id, $0)) }
    }

    public func smartQuery(_ id: Int64) throws -> String {
        var s: UnsafeMutablePointer<fc_string>?
        try check(fc_catalog_smart_query(handle, id, &s))
        defer { fc_string_free(s) }
        return String(cString: s!.pointee.value)
    }

    /// 親を変える（nil でいちばん上）。自分の中へは動かせない
    public func moveAlbum(_ id: Int64, toParent parentID: Int64?) throws {
        try check(fc_catalog_move_album(handle, id, parentID ?? 0))
    }

    /// 同じ親の中でサイドバーの並びを動かす（delta < 0 で上、> 0 で下）
    public func moveAlbumOrder(_ id: Int64, by delta: Int) throws {
        try check(fc_catalog_move_album_order(handle, id, Int32(delta)))
    }

    public func setAlbumCover(_ id: Int64, photoID: Int64?) throws {
        try check(fc_catalog_set_album_cover(handle, id, photoID ?? 0))
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

    // MARK: 写真の削除（v3.19）

    public func planDelete(_ ids: [Int64]) throws -> DeletePlan {
        var p = fc_delete_plan()
        try ids.withUnsafeBufferPointer { try check(fc_catalog_plan_delete(handle, $0.baseAddress, $0.count, &p)) }
        return DeletePlan(photos: Int(p.photos), files: Int(p.files), networkPhotos: Int(p.network_photos),
                          missingPhotos: Int(p.missing_photos))
    }

    /// 写真を削除する。ローカルのファイルは trash で（ゴミ箱へ送って成功なら true）、ネットワークボリュームは
    /// ゴミ箱を使わず完全に消す。RAW を消せた写真だけカタログから消す。時間がかかることがあるので、メインスレッド以外で呼ぶこと
    public func deletePhotos(_ ids: [Int64], trash: @escaping @Sendable (URL) -> Bool) throws -> DeleteResult {
        final class Box { let trash: @Sendable (URL) -> Bool; init(_ t: @escaping @Sendable (URL) -> Bool) { trash = t } }
        let box = Box(trash)
        let user = Unmanaged.passRetained(box).toOpaque()
        defer { Unmanaged<Box>.fromOpaque(user).release() }
        var r = fc_delete_result()
        var errors: UnsafeMutablePointer<fc_string>?
        let callback: fc_trash_fn = { user, path in
            let box = Unmanaged<Box>.fromOpaque(user!).takeUnretainedValue()
            return box.trash(URL(fileURLWithPath: String(cString: path!))) ? 0 : 1
        }
        try ids.withUnsafeBufferPointer {
            try check(fc_catalog_delete_photos(handle, $0.baseAddress, $0.count, callback, user, &r, &errors))
        }
        defer { fc_string_free(errors) }
        return DeleteResult(photosDeleted: Int(r.photos_deleted), photosFailed: Int(r.photos_failed),
                            filesTrashed: Int(r.files_trashed), filesRemoved: Int(r.files_removed),
                            filesFailed: Int(r.files_failed), errors: errors.map { String(cString: $0.pointee.value) } ?? "")
    }

    // MARK: カードの取り込み（v3.19）

    /// DCIM フォルダを持つボリューム（SD カードなど）
    public static func importSources() throws -> [ImportSource] {
        var a: UnsafeMutablePointer<fc_import_source_array>?
        try check(fc_import_sources(&a))
        defer { fc_import_source_array_free(a) }
        return UnsafeBufferPointer(start: a!.pointee.items, count: a!.pointee.count).map {
            ImportSource(volumeID: String(cString: $0.volume_id), name: String(cString: $0.name),
                         mountPoint: String(cString: $0.mount_point), dcimPath: String(cString: $0.dcim_path),
                         isRemovable: $0.removable != 0)
        }
    }

    /// path（なければいちばん近い親）があるボリュームの空き容量。取れなければ nil
    public static func freeSpace(at url: URL) -> Int64? {
        let n = url.path.withCString { fc_free_space($0) }
        return n >= 0 ? n : nil
    }

    /// カードの中身の概算。遅いカードでは時間がかかるので、メインスレッド以外で呼ぶこと
    public static func summarizeCard(_ source: URL) throws -> CardSummary {
        var s = fc_card_summary()
        try source.path.withCString { try check(fc_card_summarize($0, &s)) }
        return CardSummary(shots: Int(s.shots), files: Int(s.files), bytes: s.bytes)
    }

    public enum CardListEvent: Sendable {
        /// 撮影日時を読んだ枚数
        case progress(done: Int, total: Int)
        /// 撮影日時順の一覧
        case finished([CardShot])
    }

    /// カードの 1 枚ごとの一覧を作る（取り込み済みの判定つき。カードには書き込まない）。ストリームを途中で捨てると中断する
    public func listCardShots(source: URL, destination: URL) -> AsyncThrowingStream<CardListEvent, Error> {
        AsyncThrowingStream { continuation in
            let box = StreamTaskBox<CardListEvent>(continuation: continuation, catalog: self)
            let user = Unmanaged.passRetained(box).toOpaque()
            var task: OpaquePointer?
            let status = source.path.withCString { src in
                destination.path.withCString { dest in
                    fc_card_list_start(handle, src, dest, cardListProgress, cardListDone, user, &task)
                }
            }
            if status != FC_OK {
                Unmanaged<StreamTaskBox<CardListEvent>>.fromOpaque(user).release()
                continuation.finish(throwing: FocalError(status: status))
                return
            }
            box.setTask(task!)
            continuation.onTermination = { @Sendable _ in box.cancel() }
        }
    }

    /// カード上のファイルのサムネイル（sRGB の JPEG）をキャッシュに用意して、その URL を返す。カードを読むので、
    /// メインスレッド以外で呼ぶこと
    public static func cardThumbnail(file: URL, cacheDirectory: URL) throws -> URL {
        var out: UnsafeMutablePointer<fc_string>?
        try cacheDirectory.path.withCString { dir in
            try file.path.withCString { try check(fc_card_thumbnail(dir, $0, &out)) }
        }
        defer { fc_string_free(out) }
        return URL(fileURLWithPath: String(cString: out!.pointee.value))
    }

    public enum CardImportEvent: Sendable {
        public enum Phase: Int32, Sendable { case reading = 0, copying = 1, cataloging = 2 }
        case progress(phase: Phase, done: Int, total: Int, bytesDone: Int64, bytesTotal: Int64, current: String)
        /// 終わった。キャンセルしても、それまでにコピーした分は登録されている（result.cancelled）
        case finished(CardImportResult)
    }

    /// カードの写真をコピーして登録する。カードには書き込まない。ストリームを途中で捨てる（Task をキャンセルする）と
    /// 取り込みをキャンセルする
    public func importFromCard(_ options: CardImportOptions) -> AsyncThrowingStream<CardImportEvent, Error> {
        AsyncThrowingStream { continuation in
            let box = StreamTaskBox<CardImportEvent>(continuation: continuation, catalog: self)
            let user = Unmanaged.passRetained(box).toOpaque()
            var task: OpaquePointer?
            var opt = fc_card_import_options()
            // 取り込む写真の指定。C の文字列は、開始の呼び出しの間だけあればよい（core が複製する）
            let onlyStrings = (options.only ?? []).map { strdup($0) }
            defer { onlyStrings.forEach { free($0) } }
            let onlyPointers: [UnsafePointer<CChar>?] = onlyStrings.map { UnsafePointer($0) }
            opt.use_only = options.only == nil ? 0 : 1
            opt.only_count = onlyPointers.count
            opt.verify = options.verify ? 1 : 0
            opt.album_id = options.albumID ?? 0
            opt.tag_count = options.tagIDs.count
            let status = options.source.path.withCString { source in
                options.destination.path.withCString { dest in
                    withOptionalCString(options.thumbnailCache?.path) { cache in
                        options.tagIDs.withUnsafeBufferPointer { tags in
                            opt.source = source
                            opt.dest_root = dest
                            opt.thumbnail_cache_dir = cache
                            opt.tag_ids = tags.baseAddress
                            return withOptionalCString(options.presetID) { presetID in
                                opt.presets = options.presets?.handle
                                opt.preset_id = presetID
                                return onlyPointers.withUnsafeBufferPointer { only in
                                    opt.only_keys = only.baseAddress
                                    return fc_card_import_start(handle, &opt, cardProgress, cardDone, user, &task)
                                }
                            }
                        }
                    }
                }
            }
            if status != FC_OK {
                Unmanaged<StreamTaskBox<CardImportEvent>>.fromOpaque(user).release()
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

/// カードの取り込み中の状態。完了コールバックで解放する
private final class StreamTaskBox<Event: Sendable>: @unchecked Sendable {
    let continuation: AsyncThrowingStream<Event, Error>.Continuation
    let catalog: Catalog
    private let lock = NSLock()
    private var task: OpaquePointer?
    private var finished = false

    init(continuation: AsyncThrowingStream<Event, Error>.Continuation, catalog: Catalog) {
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

private func cardProgress(_ user: UnsafeMutableRawPointer?, _ phase: Int32, _ done: Int32, _ total: Int32,
                          _ bytesDone: Int64, _ bytesTotal: Int64, _ current: UnsafePointer<CChar>?) {
    let box = Unmanaged<StreamTaskBox<Catalog.CardImportEvent>>.fromOpaque(user!).takeUnretainedValue()
    box.continuation.yield(.progress(phase: Catalog.CardImportEvent.Phase(rawValue: phase) ?? .copying,
                                     done: Int(done), total: Int(total), bytesDone: bytesDone, bytesTotal: bytesTotal,
                                     current: String(optionalCString: current) ?? ""))
}

private func cardDone(_ user: UnsafeMutableRawPointer?, _ status: fc_status,
                      _ result: UnsafePointer<fc_card_import_result>?, _ message: UnsafePointer<CChar>?) {
    let box = Unmanaged<StreamTaskBox<Catalog.CardImportEvent>>.fromOpaque(user!).takeRetainedValue()
    let text = String(optionalCString: message) ?? ""
    // キャンセルは失敗ではなく、途中までの結果として返す
    if status == FC_OK || status == FC_ERR_CANCELLED, let result {
        box.continuation.yield(.finished(CardImportResult(result.pointee, errors: text)))
        box.continuation.finish()
    } else {
        box.continuation.finish(throwing: FocalError(status: status, message: text))
    }
    box.finish()
}

private func cardListProgress(_ user: UnsafeMutableRawPointer?, _ done: Int32, _ total: Int32) {
    let box = Unmanaged<StreamTaskBox<Catalog.CardListEvent>>.fromOpaque(user!).takeUnretainedValue()
    box.continuation.yield(.progress(done: Int(done), total: Int(total)))
}

private func cardListDone(_ user: UnsafeMutableRawPointer?, _ status: fc_status, _ shots: UnsafePointer<fc_card_shot>?,
                          _ count: Int, _ message: UnsafePointer<CChar>?) {
    let box = Unmanaged<StreamTaskBox<Catalog.CardListEvent>>.fromOpaque(user!).takeRetainedValue()
    if status == FC_OK {
        let list = (0..<count).map { CardShot(shots![$0]) }
        box.continuation.yield(.finished(list))
        box.continuation.finish()
    } else if status == FC_ERR_CANCELLED {
        box.continuation.finish()
    } else {
        box.continuation.finish(throwing: FocalError(status: status, message: String(optionalCString: message) ?? ""))
    }
    box.finish()
}
