import FocalCore
import Foundation
import Observation

/// カタログごとの設定（v3.25、design.md 7.1 章・9.4 章）: 起動時の再スキャン、カード取り込みの読み込み先。
/// カタログの中（meta）に保存するので、カタログと一緒に持ち運べ、カタログごとに別の設定にできる。
/// 設定がまだないカタログは、前の版のアプリ全体の設定（UserDefaults）があればそれを、なければ既定値を使う
@MainActor
@Observable
final class CatalogPrefs {
    private let catalog: Catalog

    /// 起動時にフォルダを再スキャンする（既定はオン）
    var rescanOnLaunch: Bool {
        didSet {
            guard rescanOnLaunch != oldValue else { return }
            try? catalog.setPreference("rescan_on_launch", rescanOnLaunch ? "1" : "0")
        }
    }

    /// カード取り込みの読み込み先。設定がなければ nil
    private(set) var importDestination: URL?

    init(catalog: Catalog) {
        self.catalog = catalog
        if let v = try? catalog.preference("rescan_on_launch") {
            rescanOnLaunch = v != "0"
        } else {
            rescanOnLaunch = UserDefaults.standard.bool(forKey: AppPaths.rescanOnLaunchKey, default: true)
        }
        if let p = try? catalog.preference("import_destination"), !p.isEmpty {
            importDestination = URL(fileURLWithPath: p)
        } else {
            importDestination = AppPaths.legacyImportDestination
        }
    }

    /// 読み込み先を記録する。nil なら、設定を消す（フォルダを外した直後など）。
    /// UI テストで環境変数 FOCAL_IMPORT_DEST を指定したときは、記録しない
    func setImportDestination(_ url: URL?) {
        importDestination = url
        guard ProcessInfo.processInfo.environment["FOCAL_IMPORT_DEST"] == nil else { return }
        try? catalog.setPreference("import_destination", url?.path)
    }
}
