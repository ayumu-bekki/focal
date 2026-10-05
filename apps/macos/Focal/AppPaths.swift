import Foundation
import UniformTypeIdentifiers

/// カタログとキャッシュの保存先（7 章・10 章）。
/// カタログは「〜.focalcatalog」パッケージ（中に catalog.sqlite と WAL）で、既定は ~/Pictures/Focal/。
/// キャッシュはシステムの既定の場所（~/Library/Caches/jp.bekki.focal/）で、カタログを切り替えても共有する
/// （キーにファイルのパス・大きさ・更新日時を含むので混ざらない）。
///
/// 環境変数（UI テストや CLI と同じ）:
/// - FOCAL_CATALOG: このカタログ（.sqlite か .focalcatalog）を開く。前回のカタログは見ない
/// - FOCAL_CATALOG_HOME: 新規カタログの既定の場所。前回のカタログ・古い場所を見ず、前回のカタログを記録しない（初回起動のテスト用）
/// - FOCAL_CACHE: サムネイルのキャッシュ。大きいプレビューはその隣の「〜-previews」
enum AppPaths {
    static let bundleID = "jp.bekki.focal"
    static let catalogExtension = "focalcatalog"
    static let catalogType = UTType(exportedAs: "jp.bekki.focal.catalog", conformingTo: .package)
    private static let databaseName = "catalog.sqlite"
    private static var env: [String: String] { ProcessInfo.processInfo.environment }

    static var fixedCatalog: URL? { env["FOCAL_CATALOG"].map { URL(fileURLWithPath: $0) } }

    /// 初回起動のテスト中（前回のカタログを読み書きしない）
    static var isolatedFromDefaults: Bool { env["FOCAL_CATALOG_HOME"] != nil || fixedCatalog != nil }

    /// 新規カタログの既定のフォルダ（~/Pictures/Focal）
    static var defaultCatalogDirectory: URL {
        if let p = env["FOCAL_CATALOG_HOME"] { return URL(fileURLWithPath: p) }
        let pictures = FileManager.default.urls(for: .picturesDirectory, in: .userDomainMask)[0]
        return pictures.appendingPathComponent("Focal", isDirectory: true)
    }

    /// 既定の場所の、まだ使われていない新規カタログの名前
    static var defaultNewCatalog: URL {
        let dir = defaultCatalogDirectory
        let base = "Focal Catalog"  // Lightroom などと同じく、表示言語に依らない名前
        var url = dir.appendingPathComponent(base).appendingPathExtension(catalogExtension)
        var n = 2
        while FileManager.default.fileExists(atPath: url.path) {
            url = dir.appendingPathComponent("\(base) \(n)").appendingPathExtension(catalogExtension)
            n += 1
        }
        return url
    }

    /// 0.2.0 までの保存先（ここにあれば初回起動の画面を出さずに開く）
    static var legacyCatalog: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return base.appendingPathComponent(bundleID).appendingPathComponent(databaseName)
    }

    /// 利用者の現像プリセット（1 つが 1 つの JSON ファイル）。FOCAL_PRESETS で変えられる（UI テスト用）
    static var presetsDirectory: URL {
        if let p = env["FOCAL_PRESETS"] { return URL(fileURLWithPath: p) }
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return base.appendingPathComponent(bundleID).appendingPathComponent("Presets", isDirectory: true)
    }

    /// アプリに同梱した現像プリセット（Focal 標準。読み取り専用）。FOCAL_BUILTIN_PRESETS で変えられる（UI テスト用）
    static var builtInPresetsDirectory: URL? {
        if let p = env["FOCAL_BUILTIN_PRESETS"] { return URL(fileURLWithPath: p) }
        return Bundle.main.resourceURL?.appendingPathComponent("Presets", isDirectory: true)
    }

    static let importPresetKey = "import.presetID"

    static func isPackage(_ catalog: URL) -> Bool { catalog.pathExtension.lowercased() == catalogExtension }

    /// カタログ（パッケージか .sqlite）の SQLite ファイル
    static func database(of catalog: URL) -> URL {
        isPackage(catalog) ? catalog.appendingPathComponent(databaseName) : catalog
    }

    /// 開ける既存のカタログか
    static func catalogExists(_ catalog: URL) -> Bool {
        FileManager.default.fileExists(atPath: database(of: catalog).path)
    }

    private static var cacheBase: URL {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent(bundleID)
    }

    static var thumbnailCache: URL {
        if let p = env["FOCAL_CACHE"] { return URL(fileURLWithPath: p) }
        return cacheBase.appendingPathComponent("thumbs")
    }

    static var previewCache: URL {
        if let p = env["FOCAL_CACHE"] { return URL(fileURLWithPath: p + "-previews") }
        return cacheBase.appendingPathComponent("previews")
    }

    // MARK: 取り込み（v3.19）

    /// 起動時にすべてのフォルダを再スキャンするか（設定）
    static let rescanOnLaunchKey = "rescanOnLaunch"
    /// 読み込み先（ライブラリのルート）の設定
    static let importDestinationKey = "importDestination"

    /// カードの読み込み先の既定: 設定 → なければ ~/Pictures/Photos
    static var savedImportDestination: URL? {
        UserDefaults.standard.string(forKey: importDestinationKey).map { URL(fileURLWithPath: $0) }
    }

    static var fallbackImportDestination: URL {
        FileManager.default.urls(for: .picturesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Photos", isDirectory: true)
    }
}
