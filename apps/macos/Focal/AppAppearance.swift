import AppKit
import SwiftUI

/// アプリの外観（設定ウィンドウで選ぶ）。システムに合わせるか、ライト・ダークに固定する
enum AppAppearance: String, CaseIterable, Identifiable {
    case system, light, dark

    static let key = "Appearance"
    /// まだ外観を選んでいないときの既定（v3.26: ダーク。写真を見るアプリなので、周りの色の影響が少ない暗い表示を標準にする）
    static let defaultValue = AppAppearance.dark
    var id: String { rawValue }

    var title: LocalizedStringKey {
        switch self {
        case .system: "Use System Setting"
        case .light: "Light"
        case .dark: "Dark"
        }
    }

    /// 保存された設定（なければ既定のダーク）。UI テスト（FOCAL_WINDOW_SIZE 指定）では保存値を使わず、FOCAL_APPEARANCE で指定する
    /// （指定がなければライト。ユーザーガイドの画面などの撮影を変えないため）
    static var current: AppAppearance {
        let env = ProcessInfo.processInfo.environment
        if env["FOCAL_WINDOW_SIZE"] != nil {
            return env["FOCAL_APPEARANCE"].flatMap(AppAppearance.init(rawValue:)) ?? .light
        }
        return UserDefaults.standard.string(forKey: key).flatMap(AppAppearance.init(rawValue:)) ?? defaultValue
    }

    /// すべてのウィンドウに適用する
    @MainActor
    func apply() {
        switch self {
        case .system: NSApp.appearance = nil
        case .light: NSApp.appearance = NSAppearance(named: .aqua)
        case .dark: NSApp.appearance = NSAppearance(named: .darkAqua)
        }
    }
}
