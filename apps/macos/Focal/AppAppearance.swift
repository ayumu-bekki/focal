import AppKit
import SwiftUI

/// アプリの外観（設定ウィンドウで選ぶ）。システムに合わせるか、ライト・ダークに固定する
enum AppAppearance: String, CaseIterable, Identifiable {
    case system, light, dark

    static let key = "Appearance"
    var id: String { rawValue }

    var title: LocalizedStringKey {
        switch self {
        case .system: "Use System Setting"
        case .light: "Light"
        case .dark: "Dark"
        }
    }

    /// 保存された設定。UI テスト（FOCAL_WINDOW_SIZE 指定）では保存値を使わず、FOCAL_APPEARANCE で指定する
    static var current: AppAppearance {
        let env = ProcessInfo.processInfo.environment
        if env["FOCAL_WINDOW_SIZE"] != nil {
            return env["FOCAL_APPEARANCE"].flatMap(AppAppearance.init(rawValue:)) ?? .system
        }
        return UserDefaults.standard.string(forKey: key).flatMap(AppAppearance.init(rawValue:)) ?? .system
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
