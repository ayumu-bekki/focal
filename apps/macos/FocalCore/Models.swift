import CFocal
import Foundation

public struct PhotoRoot: Identifiable, Hashable, Sendable {
    public let id: Int64
    public let path: String
    public let label: String
    /// v3.19: ボリュームの ID・名前と、ボリュームのルートからの相対パス（判別できなければ空）
    public let volumeID: String
    public let volumeName: String
    public let volumeRelativePath: String
    /// いまアクセスできる（外付けドライブが外れていると false）
    public let isOnline: Bool

    /// 表示名: 付けた名前、なければフォルダ名
    public var displayName: String {
        if !label.isEmpty { return label }
        return URL(fileURLWithPath: path).lastPathComponent
    }
}

/// ルートの詳しい情報（サイドバーの「情報を見る」）
public struct RootDetails: Sendable {
    public enum Kind: Int32, Sendable { case unknown = 0, `internal` = 1, external = 2, network = 3 }

    public let id: Int64
    public let path: String
    public let label: String
    public let volumeID: String
    public let volumeName: String
    /// ボリュームのマウントポイント。オフラインなどでわからなければ空
    public let mountPoint: String
    /// ファイルシステム。わからなければ空
    public let fileSystem: String
    public let isOnline: Bool
    public let kind: Kind
    /// ボリュームの容量。取れなければ nil
    public let totalBytes: Int64?
    public let freeBytes: Int64?
    public let photos: Int64
    public let folders: Int64
    public let missing: Int64
    /// 現像・★・フラグ・タグのいずれかが付いた写真の枚数（カタログから外すと消える情報の目安）
    public let editedPhotos: Int64
    public let totalFileBytes: Int64
    /// 撮影日の範囲（"YYYY-MM-DD"）。なければ nil
    public let captureFrom: String?
    public let captureTo: String?
}

public struct PhotoFolder: Identifiable, Hashable, Sendable {
    public let id: Int64
    public let rootID: Int64
    public let parentID: Int64?
    /// ルートからの相対パス（ルート自身は ""）
    public let relativePath: String
    /// このフォルダ直下の写真の数
    public let photoCount: Int64

    public var name: String { relativePath.split(separator: "/").last.map(String.init) ?? "" }
}

public struct PhotoTag: Identifiable, Hashable, Sendable {
    public let id: Int64
    public let parentID: Int64?
    public let name: String
    /// "親/子"
    public let path: String
    public let photoCount: Int64
}

/// アルバム（v3.16）: 利用者が選んだ写真の集まり。v3.19 でフォルダ（入れ子）とスマートアルバムを追加
public struct Album: Identifiable, Hashable, Sendable {
    public enum Kind: Int32, Sendable {
        /// 手で集める
        case album = 0
        /// アルバムを入れる入れ物
        case folder = 1
        /// 保存した検索条件（読み取り専用）
        case smart = 2
    }

    public let id: Int64
    public let name: String
    /// フォルダは 0。スマートアルバムは条件に合う枚数
    public let photoCount: Int64
    public let parentID: Int64?
    public let kind: Kind
    public let coverPhotoID: Int64?

    /// 写真を足せるのは手で集めるアルバムだけ
    public var acceptsPhotos: Bool { kind == .album }
}

public enum PhotoStatus: Int32, Sendable {
    case ok = 0, missing = 1, unsupported = 2
}

/// 写真のファイルの種類（v3.22）。RAW 以外は現像できない（表示・★・タグ・アルバムなどは同じ）
public enum PhotoKind: Int32, Sendable {
    case raw = 0, jpeg = 1, tiff = 2, png = 3, heif = 4

    public var isDevelopable: Bool { self == .raw }
}

public enum PhotoFlag: Int32, Sendable {
    case rejected = -1, none = 0, picked = 1
}

public struct Photo: Identifiable, Hashable, Sendable {
    public let id: Int64
    public let folderID: Int64
    public let fileName: String
    public let path: String
    public let status: PhotoStatus
    /// "YYYY-MM-DDTHH:MM:SS"（ローカル時刻）
    public let captureTime: String?
    public let cameraMake: String
    public let cameraModel: String
    public let lensModel: String
    public let iso: Int64?
    public let exposureTime: Double?
    public let fNumber: Double?
    public let focalLength: Double?
    public let width: Int
    public let height: Int
    public let orientation: Int
    public var rating: Int
    public var flag: PhotoFlag
    public let fileSize: Int64
    public let fileMTime: Int64
    /// 主役のファイルの種類
    public let kind: PhotoKind
    /// 同じ名前（拡張子を除く）の付属の写真ファイル（RAW の JPEG など）。主役と同じ 1 枚として扱う
    public let companions: [String]

    init(_ c: fc_photo) {
        id = c.id
        folderID = c.folder_id
        fileName = String(cString: c.file_name)
        path = String(cString: c.path)
        status = PhotoStatus(rawValue: c.status) ?? .unsupported
        captureTime = String(optionalCString: c.capture_time)
        cameraMake = String(cString: c.camera_make)
        cameraModel = String(cString: c.camera_model)
        lensModel = String(cString: c.lens_model)
        iso = c.iso > 0 ? c.iso : nil
        exposureTime = c.exposure_time > 0 ? c.exposure_time : nil
        fNumber = c.f_number > 0 ? c.f_number : nil
        focalLength = c.focal_length > 0 ? c.focal_length : nil
        width = Int(c.width)
        height = Int(c.height)
        orientation = Int(c.orientation)
        rating = Int(c.rating)
        flag = PhotoFlag(rawValue: c.flag) ?? .none
        fileSize = c.file_size
        fileMTime = c.file_mtime
        kind = PhotoKind(rawValue: c.kind) ?? .raw
        companions = String(cString: c.companions).split(separator: "/").map(String.init)
    }
}

public struct PhotoFilter: Hashable, Sendable {
    public enum Flag: Int32, Sendable, CaseIterable {
        case any = 0, picked, rejected, unflagged, notRejected
    }

    public var folderID: Int64?
    public var includeSubfolders = true
    public var minRating = 0
    public var flag: Flag = .any
    public var tagID: Int64?
    /// "YYYY-MM-DD"
    public var dateFrom: String?
    public var dateTo: String?
    public var includeUnavailable = true
    /// このアルバムの写真だけ（v3.16）
    public var albumID: Int64?
    /// 最後に写真を足した取り込みの写真だけ（v3.16）
    public var recentImport = false
    /// このスマートアルバムの条件に合う写真だけ（v3.19）
    public var smartAlbumID: Int64?

    public init() {}

    /// C の構造体にして body を呼ぶ（文字列のポインタは body の間だけ有効）
    func withCFilter<R>(_ body: (UnsafePointer<fc_photo_filter>) throws -> R) rethrows -> R {
        var f = fc_photo_filter()
        fc_photo_filter_init(&f)
        f.folder_id = folderID ?? 0
        f.include_subfolders = includeSubfolders ? 1 : 0
        f.min_rating = Int32(minRating)
        f.flag = flag.rawValue
        f.tag_id = tagID ?? 0
        f.include_unavailable = includeUnavailable ? 1 : 0
        f.album_id = albumID ?? 0
        f.recent_import = recentImport ? 1 : 0
        f.smart_album_id = smartAlbumID ?? 0
        return try withOptionalCString(dateFrom) { from in
            try withOptionalCString(dateTo) { to in
                f.date_from = from
                f.date_to = to
                return try body(&f)
            }
        }
    }
}

/// カタログから外したときに保管して、まだ写真につながっていない情報（v3.23）
public struct DetachedSummary: Sendable, Equatable {
    public let items: Int
    public let edits: Int

    public init(items: Int, edits: Int) {
        self.items = items
        self.edits = edits
    }
}

/// カタログの情報（「カタログ情報」の画面用、v3.23）
public struct CatalogInfo: Sendable, Equatable {
    public let fileBytes: Int64
    public let schemaVersion: Int
    public let roots: Int, folders: Int
    public let photos: Int, rawPhotos: Int, missingPhotos: Int
    public let editedPhotos: Int
    public let albums: Int, tags: Int
    /// 外したフォルダの保管情報（写真の数・うち現像の設定がある数）
    public let detachedItems: Int, detachedEdits: Int
    /// カタログの隣のバックアップ（*.bak）
    public let backupFiles: Int, backupBytes: Int64

    init(_ c: fc_catalog_info) {
        fileBytes = c.file_bytes
        schemaVersion = Int(c.schema_version)
        roots = Int(c.roots)
        folders = Int(c.folders)
        photos = Int(c.photos)
        rawPhotos = Int(c.photos_raw)
        missingPhotos = Int(c.photos_missing)
        editedPhotos = Int(c.edited_photos)
        albums = Int(c.albums)
        tags = Int(c.tags)
        detachedItems = Int(c.detached_items)
        detachedEdits = Int(c.detached_edits)
        backupFiles = Int(c.backup_files)
        backupBytes = c.backup_bytes
    }
}

public struct OptimizeResult: Sendable {
    public let removedItems: Int
    public let bytesBefore: Int64
    public let bytesAfter: Int64
}

public struct ScanStats: Sendable {
    public var added = 0, updated = 0, unchanged = 0, missing = 0, restored = 0, renamed = 0, relinked = 0, inherited = 0
    /// カタログから外したときに保管した情報を、同じ写真に戻した数（v3.23）
    public var restoredData = 0
    public var unsupported = 0, foldersAdded = 0, thumbnails = 0, thumbnailFailures = 0

    init(_ s: fc_scan_stats) {
        added = Int(s.added)
        updated = Int(s.updated)
        unchanged = Int(s.unchanged)
        missing = Int(s.missing)
        restored = Int(s.restored)
        renamed = Int(s.renamed)
        relinked = Int(s.relinked)
        inherited = Int(s.inherited)
        restoredData = Int(s.restored_data)
        unsupported = Int(s.unsupported)
        foldersAdded = Int(s.folders_added)
        thumbnails = Int(s.thumbnails)
        thumbnailFailures = Int(s.thumbnail_failures)
    }
}

func withOptionalCString<R>(_ s: String?, _ body: (UnsafePointer<CChar>?) throws -> R) rethrows -> R {
    if let s { return try s.withCString { try body($0) } }
    return try body(nil)
}

// MARK: 写真の削除（v3.19）

/// 削除の前に数えたもの。写真 = カタログの RAW と、同じフォルダで同じ名前の幹の JPEG・動画・サイドカー
public struct DeletePlan: Sendable, Equatable {
    public let photos: Int
    /// 消すファイルの数（すでにないものは数えない）
    public let files: Int
    /// ネットワークボリューム（ゴミ箱を使わず完全に消える）にある写真の数
    public let networkPhotos: Int
    /// ファイルがすでにない写真（カタログの情報だけ消える）
    public let missingPhotos: Int
}

public struct DeleteResult: Sendable {
    public let photosDeleted: Int
    /// RAW を消せなかったので、ファイルもカタログも残した写真
    public let photosFailed: Int
    public let filesTrashed: Int
    /// 完全に削除したファイル
    public let filesRemoved: Int
    public let filesFailed: Int
    /// 失敗の内容（改行区切り）
    public let errors: String
}

// MARK: カードの取り込み（v3.19）

/// DCIM フォルダを持つボリューム（SD カードなど）
public struct ImportSource: Identifiable, Hashable, Sendable {
    public var id: String { mountPoint }
    public let volumeID: String
    public let name: String
    public let mountPoint: String
    public let dcimPath: String
    public let isRemovable: Bool
}

public struct CardSummary: Hashable, Sendable {
    /// RAW + JPEG のペアなどは 1 枚
    public let shots: Int
    public let files: Int
    public let bytes: Int64
}

public struct CardImportOptions: Sendable {
    public var source: URL
    /// コピー先。<destination>/YYYY/YYYY-MM-DD/ に入れる
    public var destination: URL
    public var verify = true
    public var albumID: Int64?
    public var tagIDs: [Int64] = []
    public var thumbnailCache: URL?
    /// 取り込んだ写真に重ねる現像のプリセット（presets から引く。nil なら「なし」）
    public var presetID: String?
    public var presets: PresetStore?
    /// 取り込む写真の指定（`CardShot.key`）。nil ならカードの全部（取り込み済みは除く）。空なら何も取り込まない
    public var only: [String]?

    public init(source: URL, destination: URL) {
        self.source = source
        self.destination = destination
    }
}

public struct CardImportResult: Sendable {
    public var shots = 0, imported = 0, skippedDuplicates = 0, failed = 0, estimatedDates = 0, filesCopied = 0
    /// 取り込む写真の指定（`only`）に入っていないのでコピーしなかった枚数（取り込み済みは数えない）
    public var skippedUnselected = 0
    public var bytesCopied: Int64 = 0
    public var cancelled = false
    public var rootID: Int64?
    /// カタログに新しく足した写真の数
    public var added = 0
    /// コピーするはずだった大きさ（取り込み済みを除く）と、読み込み先の空き容量（取れなければ nil）
    public var bytesNeeded: Int64 = 0
    public var spaceAvailable: Int64?
    /// 失敗した写真のメッセージ（改行区切り）
    public var errors = ""

    init(_ r: fc_card_import_result, errors: String) {
        shots = Int(r.shots)
        imported = Int(r.imported)
        skippedDuplicates = Int(r.skipped_duplicates)
        skippedUnselected = Int(r.skipped_unselected)
        failed = Int(r.failed)
        estimatedDates = Int(r.estimated_dates)
        filesCopied = Int(r.files_copied)
        bytesCopied = r.bytes_copied
        cancelled = r.cancelled != 0
        rootID = r.root_id > 0 ? r.root_id : nil
        added = Int(r.added)
        bytesNeeded = r.bytes_needed
        spaceAvailable = r.space_available >= 0 ? r.space_available : nil
        self.errors = errors
    }
}

/// カードの 1 枚（取り込む写真を選ぶ一覧用。RAW + JPEG のペアやサイドカーは 1 枚）
public struct CardShot: Sendable, Identifiable, Hashable {
    /// 1 枚を指す鍵（`CardImportOptions.only` に渡す）
    public var id: String { key }
    public var key: String
    /// 表示名（写真になるファイル。RAW があれば RAW）
    public var name: String
    /// サムネイルを取り出すファイル
    public var path: URL
    /// 'YYYY-MM-DDTHH:MM:SS'
    public var captureTime: String
    /// 撮影日時を読めず、ファイルの更新日時で代用した
    public var estimated: Bool
    public var files: Int
    public var bytes: Int64
    public var hasRaw: Bool
    /// 同じ名前の JPEG などが一緒にある
    public var hasCompanion: Bool
    /// false なら動画だけ（カタログには登録されないが、コピーはされる）
    public var isPhoto: Bool
    /// 取り込み済み（取り込みでは飛ばされる）
    public var imported: Bool

    /// 撮影日（'YYYY-MM-DD'）
    public var day: String { String(captureTime.prefix(10)) }

    init(_ c: fc_card_shot) {
        key = String(cString: c.key)
        name = String(cString: c.name)
        path = URL(fileURLWithPath: String(cString: c.path))
        captureTime = String(cString: c.capture_time)
        estimated = c.estimated != 0
        files = Int(c.files)
        bytes = c.bytes
        hasRaw = c.has_raw != 0
        hasCompanion = c.has_companion != 0
        isPhoto = c.is_photo != 0
        imported = c.imported != 0
    }
}
