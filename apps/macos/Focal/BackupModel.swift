import AppKit
import FocalCore
import SwiftUI

/// カタログの定期バックアップの設定（v3.24、design.md 7.1 章）。アプリ全体の設定で、どのカタログにも効く。
/// 前回のバックアップの日時だけは、カタログごと（カタログ自身に記録する）
enum BackupSettings {
    static let intervalKey = "backup.intervalDays"
    static let askKey = "backup.askOnQuit"
    static let keepKey = "backup.keep"
    static let checkKey = "backup.checkIntegrity"
    static let locationKey = "backup.location"

    /// 設定のプルダウンの選択肢: 0 = 終了時に毎回、N = 前回から N 日以上たったら終了時に、-1 = 自動ではしない
    static let intervals = [0, 1, 3, 7, 14, 30, -1]
    static let defaultInterval = 7
    /// 残す世代の選択肢（0 = すべて残す）
    static let keeps = [3, 5, 10, 20, 50, 0]
    static let defaultKeep = 10

    private static var env: [String: String] { ProcessInfo.processInfo.environment }

    /// 終了時に自動でバックアップを考えるか。UI テスト（固定のカタログ）では、環境変数で指定したときだけ
    static var autoOnQuitEnabled: Bool { !AppPaths.isolatedFromDefaults || env["FOCAL_BACKUP_INTERVAL"] != nil }

    static var intervalDays: Int {
        if let e = env["FOCAL_BACKUP_INTERVAL"], let n = Int(e) { return n }
        return UserDefaults.standard.object(forKey: intervalKey) as? Int ?? defaultInterval
    }

    static var askOnQuit: Bool {
        if let e = env["FOCAL_BACKUP_ASK"] { return e != "0" }
        return UserDefaults.standard.object(forKey: askKey) as? Bool ?? true
    }

    static var keep: Int { UserDefaults.standard.object(forKey: keepKey) as? Int ?? defaultKeep }
    static var checkIntegrity: Bool { UserDefaults.standard.object(forKey: checkKey) as? Bool ?? true }

    /// バックアップの保存先（既定は ~/Pictures/Focal/Backups。別のディスクを選べる）
    static var location: URL {
        if let e = env["FOCAL_BACKUP_DIR"] { return URL(fileURLWithPath: e) }
        if let p = UserDefaults.standard.string(forKey: locationKey), !p.isEmpty { return URL(fileURLWithPath: p) }
        return defaultLocation
    }

    static var defaultLocation: URL {
        FileManager.default.urls(for: .picturesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Focal", isDirectory: true).appendingPathComponent("Backups", isDirectory: true)
    }

    static func title(forInterval days: Int) -> LocalizedStringKey {
        switch days {
        case 0: "Every time Focal quits"
        case 1: "Once a day"
        case 3: "Every 3 days"
        case 7: "Once a week"
        case 14: "Every 2 weeks"
        case 30: "Once a month"
        default: "Never (manual only)"
        }
    }
}

/// カタログのバックアップの状態と操作。設定画面の「今すぐバックアップ」と、終了時の流れで共通
@MainActor
@Observable
final class BackupModel {
    enum Phase: Equatable {
        case idle
        case running
        case finished(String)  // 結果のメッセージ
        case failed(String)
    }

    /// 終了時の流れ（パネルに出す）
    enum QuitStep: Equatable {
        case asking
        case running
        case failed(String)
        case damaged(String)
    }

    private(set) var phase: Phase = .idle
    private(set) var progress: Double = 0
    private(set) var lastBackup: Date?
    private(set) var items: [BackupItem] = []
    /// 整合性の確認に失敗して取らなかったときの SQLite の報告（設定画面から取ったとき。「それでも取る」を聞く）
    var damagedMessage: String?
    var quitStep: QuitStep?

    private weak var model: LibraryModel?
    private var task: Task<Void, Never>?
    private var quitCompletion: ((Bool) -> Void)?

    func configure(model: LibraryModel) { self.model = model }

    var isRunning: Bool { phase == .running }

    /// 前回の日時と、保存先の最近のバックアップを読み直す
    func refresh() {
        guard let catalog = model?.catalog else { return }
        let dir = BackupSettings.location
        Task {
            let result = await Task.detached { () -> (Date?, [BackupItem]) in
                (try? catalog.lastBackupDate(), (try? catalog.backups(in: dir)) ?? [])
            }.value
            lastBackup = result.0
            items = result.1
        }
    }

    /// 前回の日時から、いまが取る時期か（設定の頻度で）
    var isDue: Bool {
        let days = BackupSettings.intervalDays
        guard days >= 0, let catalog = model?.catalog else { return false }
        return (try? catalog.isBackupDue(intervalDays: days)) ?? false
    }

    /// 前回から何日たったか（取ったことがなければ nil）
    var daysSinceLast: Int? {
        lastBackup.map { max(0, Calendar.current.dateComponents([.day], from: $0, to: Date()).day ?? 0) }
    }

    private enum RunResult {
        case done(String)
        case skippedDamaged(String)
        case failed(String)
        case cancelled
    }

    /// バックアップを取る（設定の保存先・世代・整合性の確認で）。開いている写真の編集を保存し、書き込みを終わらせてから取る
    private func run(allowDamaged: Bool) async -> RunResult {
        guard let model else { return .failed("") }
        phase = .running
        progress = 0
        model.develop.saveNow()
        let catalog = model.catalog
        let dir = BackupSettings.location
        let check = BackupSettings.checkIntegrity
        let keep = BackupSettings.keep
        let outcome: RunResult = await withCheckedContinuation { continuation in
            task = Task {
                do {
                    try await Task.detached { try catalog.flush() }.value
                    var result: BackupOutcome?
                    for try await ev in catalog.backup(to: dir, checkIntegrity: check, allowDamaged: allowDamaged) {
                        switch ev {
                        case .progress(let f): progress = f
                        case .finished(let r): result = r
                        }
                    }
                    guard let result else {
                        continuation.resume(returning: .failed(""))
                        return
                    }
                    if result.skippedDamaged {
                        continuation.resume(returning: .skippedDamaged(result.message))
                    } else {
                        _ = await Task.detached { try? catalog.pruneBackups(in: dir, keep: keep) }.value
                        continuation.resume(returning: .done(result.url?.lastPathComponent ?? ""))
                    }
                } catch let e as FocalError where e.code == .cancelled {
                    continuation.resume(returning: .cancelled)
                } catch {
                    continuation.resume(returning: .failed(String(describing: error)))
                }
            }
        }
        task = nil
        refresh()
        return outcome
    }

    /// 設定画面の「今すぐバックアップ」
    func backUpNow(allowDamaged: Bool = false) {
        guard !isRunning else { return }
        damagedMessage = nil
        Task {
            switch await run(allowDamaged: allowDamaged) {
            case .done(let name):
                phase = .finished(String(localized: "Backed up: \(name)"))
            case .skippedDamaged(let message):
                phase = .idle
                damagedMessage = message
            case .failed(let message):
                phase = .failed(message)
            case .cancelled:
                phase = .idle
            }
        }
    }

    func cancel() { task?.cancel() }

    // MARK: 終了時の流れ（Lightroom Classic と同じ）

    /// 終了の途中でバックアップを聞く・取る。completion(true) で終了、false で終了をやめる
    func beginQuitFlow(completion: @escaping (Bool) -> Void) {
        quitCompletion = completion
        if BackupSettings.askOnQuit {
            quitStep = .asking
        } else {
            quitBackup(allowDamaged: false)
        }
    }

    func quitBackup(allowDamaged: Bool) {
        quitStep = .running
        Task {
            switch await run(allowDamaged: allowDamaged) {
            case .done: finishQuit(true)
            case .skippedDamaged(let message): quitStep = .damaged(message)
            case .failed(let message): quitStep = .failed(message)
            case .cancelled: finishQuit(false)
            }
        }
    }

    /// 今回はバックアップせずに終了する
    func quitSkip() { finishQuit(true) }

    /// 終了をやめて、アプリに戻る
    func quitCancel() {
        if isRunning { cancel() } else { finishQuit(false) }
    }

    private func finishQuit(_ quit: Bool) {
        quitStep = nil
        phase = .idle
        let completion = quitCompletion
        quitCompletion = nil
        completion?(quit)
    }
}

/// 終了時のバックアップのパネル。ウィンドウを閉じて終了するときも出せるように、ウィンドウに付けないパネルにする
@MainActor
final class BackupQuitController {
    private var panel: NSPanel?

    func present(_ backup: BackupModel) {
        let panel = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 440, height: 200),
                            styleMask: [.titled], backing: .buffered, defer: false)
        panel.title = String(localized: "Back Up Catalog")
        panel.isReleasedWhenClosed = false
        panel.level = .modalPanel
        panel.contentViewController = NSHostingController(rootView: BackupQuitView(backup: backup))
        panel.setContentSize(panel.contentViewController?.view.fittingSize ?? NSSize(width: 440, height: 200))
        panel.center()
        panel.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        self.panel = panel
    }

    func dismiss() {
        panel?.orderOut(nil)
        panel = nil
    }
}

/// 終了時のバックアップのパネルの中身
struct BackupQuitView: View {
    let backup: BackupModel
    @State private var dontAskAgain = false

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            switch backup.quitStep {
            case .asking, .none:
                Text("Back up the catalog before quitting?").font(.headline)
                Text(lastBackupText).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                Text("Backups are saved in \(BackupSettings.location.path).")
                    .font(.caption).foregroundStyle(.secondary).lineLimit(2).truncationMode(.middle)
                Toggle("Back up automatically next time, without asking", isOn: $dontAskAgain)
                    .accessibilityIdentifier("backupDontAsk")
                HStack {
                    Button("Cancel") { backup.quitCancel() }
                        .keyboardShortcut(.cancelAction)
                        .accessibilityIdentifier("backupQuitCancel")
                    Spacer()
                    Button("Skip and Quit") { backup.quitSkip() }
                        .accessibilityIdentifier("backupQuitSkip")
                    Button("Back Up and Quit") {
                        if dontAskAgain { UserDefaults.standard.set(false, forKey: BackupSettings.askKey) }
                        backup.quitBackup(allowDamaged: false)
                    }
                    .keyboardShortcut(.defaultAction)
                    .accessibilityIdentifier("backupQuitStart")
                }
            case .running:
                Text("Backing up the catalog…").font(.headline)
                ProgressView(value: backup.progress)
                    .accessibilityIdentifier("backupProgress")
                Text("Focal quits when the backup is finished.").font(.caption).foregroundStyle(.secondary)
                HStack {
                    Spacer()
                    Button("Cancel") { backup.quitCancel() }
                        .accessibilityIdentifier("backupQuitCancel")
                }
            case .failed(let message):
                Text("The backup failed").font(.headline)
                Text(message).font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                HStack {
                    Button("Cancel") { backup.quitCancel() }
                        .keyboardShortcut(.cancelAction)
                    Spacer()
                    Button("Quit Anyway") { backup.quitSkip() }
                        .accessibilityIdentifier("backupQuitSkip")
                }
            case .damaged(let message):
                Text("The catalog may be damaged").font(.headline)
                Text("The integrity check found a problem with the catalog. A backup of a damaged catalog counts toward the number of backups kept, so older, good backups may be deleted.")
                    .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                Text(message).font(.caption).foregroundStyle(.secondary).lineLimit(4)
                HStack {
                    Button("Cancel") { backup.quitCancel() }
                        .keyboardShortcut(.cancelAction)
                    Spacer()
                    Button("Skip and Quit") { backup.quitSkip() }
                        .accessibilityIdentifier("backupQuitSkip")
                    Button("Back Up Anyway") { backup.quitBackup(allowDamaged: true) }
                        .accessibilityIdentifier("backupQuitAnyway")
                }
            }
        }
        .padding(20)
        .frame(width: 440)
        .accessibilityIdentifier("backupQuitPanel")
    }

    private var lastBackupText: String {
        if let days = backup.daysSinceLast {
            return days == 0 ? String(localized: "The last backup was made today.")
                             : String(localized: "The last backup was \(days) days ago.")
        }
        return String(localized: "This catalog has not been backed up yet.")
    }
}
