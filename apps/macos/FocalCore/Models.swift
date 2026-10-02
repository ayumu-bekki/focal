import CFocal
import Foundation

public struct PhotoRoot: Identifiable, Hashable, Sendable {
    public let id: Int64
    public let path: String
    public let label: String
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

/// アルバム（v3.16）: 利用者が選んだ写真の集まり
public struct Album: Identifiable, Hashable, Sendable {
    public let id: Int64
    public let name: String
    public let photoCount: Int64
}

public enum PhotoStatus: Int32, Sendable {
    case ok = 0, missing = 1, unsupported = 2
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
        return try withOptionalCString(dateFrom) { from in
            try withOptionalCString(dateTo) { to in
                f.date_from = from
                f.date_to = to
                return try body(&f)
            }
        }
    }
}

public struct ScanStats: Sendable {
    public var added = 0, updated = 0, unchanged = 0, missing = 0, restored = 0, renamed = 0
    public var unsupported = 0, foldersAdded = 0, thumbnails = 0, thumbnailFailures = 0

    init(_ s: fc_scan_stats) {
        added = Int(s.added)
        updated = Int(s.updated)
        unchanged = Int(s.unchanged)
        missing = Int(s.missing)
        restored = Int(s.restored)
        renamed = Int(s.renamed)
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
