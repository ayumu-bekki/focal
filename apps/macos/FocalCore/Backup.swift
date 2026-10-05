import CFocal
import Foundation

/// 定期バックアップ 1 件（v3.24）。そのまま開けるカタログのパッケージ
public struct BackupItem: Identifiable, Hashable, Sendable {
    public var id: URL { url }
    public let url: URL
    /// "YYYY-MM-DD HH:MM"（名前から）
    public let created: String
    public let bytes: Int64
}

/// バックアップを取った結果
public struct BackupOutcome: Sendable {
    /// 作ったバックアップ。整合性の確認に失敗して取らなかったときは nil
    public let url: URL?
    public let bytes: Int64
    /// 整合性の確認に失敗したので取らなかった（message に SQLite の報告）
    public let skippedDamaged: Bool
    public let message: String
}

public enum BackupEvent: Sendable {
    /// 0...1
    case progress(Double)
    case finished(BackupOutcome)
}

extension Catalog {
    /// カタログごとの設定（v3.25。カタログと一緒に持ち運べる）。設定がなければ nil。name は小文字・数字・"_" だけ
    public func preference(_ name: String) throws -> String? {
        var s: UnsafeMutablePointer<fc_string>?
        try name.withCString { try check(fc_catalog_get_preference(handle, $0, &s)) }
        guard let s else { return nil }
        defer { fc_string_free(s) }
        return String(cString: s.pointee.value)
    }

    /// カタログごとの設定を書く。value が nil なら消す（既定に戻る）
    public func setPreference(_ name: String, _ value: String?) throws {
        try name.withCString { n in try withOptionalCString(value) { try check(fc_catalog_set_preference(handle, n, $0)) } }
    }

    /// カタログの名前（バックアップの名前に使う）
    public func name() throws -> String {
        var s: UnsafeMutablePointer<fc_string>?
        try check(fc_catalog_name(handle, &s))
        defer { fc_string_free(s) }
        return String(cString: s!.pointee.value)
    }

    /// 最後に定期バックアップを取った日時。取ったことがなければ nil
    public func lastBackupDate() throws -> Date? {
        var s: UnsafeMutablePointer<fc_string>?
        try check(fc_catalog_last_backup_at(handle, &s))
        guard let s else { return nil }
        defer { fc_string_free(s) }
        let f = ISO8601DateFormatter()
        return f.date(from: String(cString: s.pointee.value))
    }

    /// 前回から intervalDays 日以上たっている（または一度も取っていない）か。0 は常に true（毎回）、負は常に false（自動なし）
    public func isBackupDue(intervalDays: Int) throws -> Bool {
        var due: Int32 = 0
        try check(fc_catalog_backup_due(handle, Int32(intervalDays), &due))
        return due != 0
    }

    /// dir の中の、このカタログのバックアップを新しい順に
    public func backups(in dir: URL) throws -> [BackupItem] {
        let name = try name()
        var a: UnsafeMutablePointer<fc_backup_array>?
        try dir.path.withCString { d in try name.withCString { try check(fc_backups_list(d, $0, &a)) } }
        defer { fc_backup_array_free(a) }
        return UnsafeBufferPointer(start: a!.pointee.items, count: a!.pointee.count).map {
            BackupItem(url: URL(fileURLWithPath: String(cString: $0.path)), created: String(cString: $0.created), bytes: $0.bytes)
        }
    }

    /// 新しい keep 個を残して、古いバックアップを消す（keep <= 0 は何も消さない）。消した数
    @discardableResult
    public func pruneBackups(in dir: URL, keep: Int) throws -> Int {
        let name = try name()
        var removed: Int32 = 0
        try dir.path.withCString { d in try name.withCString { try check(fc_backups_prune(d, $0, Int32(keep), &removed)) } }
        return Int(removed)
    }

    /// dir にバックアップを取る。ストリームを途中で捨てる（Task をキャンセルする）と、取るのを打ち切り、書きかけは消える。
    /// 成功すると、最後のバックアップの日時をカタログに記録する
    public func backup(to dir: URL, checkIntegrity: Bool = true, allowDamaged: Bool = false)
        -> AsyncThrowingStream<BackupEvent, Error> {
        AsyncThrowingStream { continuation in
            let box = BackupBox(continuation: continuation, catalog: self)
            let user = Unmanaged.passRetained(box).toOpaque()
            var task: OpaquePointer?
            let status = dir.path.withCString { d in
                var opt = fc_backup_options()
                opt.dest_dir = d
                opt.check_integrity = checkIntegrity ? 1 : 0
                opt.allow_damaged = allowDamaged ? 1 : 0
                return fc_catalog_backup_start(handle, &opt, backupProgress, backupDone, user, &task)
            }
            if status != FC_OK {
                Unmanaged<BackupBox>.fromOpaque(user).release()
                continuation.finish(throwing: FocalError(status: status))
                return
            }
            box.setTask(task!)
            continuation.onTermination = { @Sendable _ in box.cancel() }
        }
    }
}

/// バックアップ中の状態。完了コールバックで解放する
private final class BackupBox: @unchecked Sendable {
    let continuation: AsyncThrowingStream<BackupEvent, Error>.Continuation
    let catalog: Catalog  // バックアップ中はカタログを閉じない
    private let lock = NSLock()
    private var task: OpaquePointer?
    private var finished = false

    init(continuation: AsyncThrowingStream<BackupEvent, Error>.Continuation, catalog: Catalog) {
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
        DispatchQueue.global(qos: .utility).async { fc_task_release(OpaquePointer(bitPattern: handle)) }
    }
}

private func backupProgress(_ user: UnsafeMutableRawPointer?, _ fraction: Double) {
    let box = Unmanaged<BackupBox>.fromOpaque(user!).takeUnretainedValue()
    box.continuation.yield(.progress(fraction))
}

private func backupDone(_ user: UnsafeMutableRawPointer?, _ status: fc_status, _ result: UnsafePointer<fc_backup_result>?,
                        _ path: UnsafePointer<CChar>?, _ message: UnsafePointer<CChar>?) {
    let box = Unmanaged<BackupBox>.fromOpaque(user!).takeRetainedValue()
    if status == FC_OK {
        let p = String(cString: path!)
        box.continuation.yield(.finished(BackupOutcome(
            url: p.isEmpty ? nil : URL(fileURLWithPath: p), bytes: result!.pointee.bytes,
            skippedDamaged: result!.pointee.skipped_damaged != 0, message: String(cString: message!))))
        box.continuation.finish()
    } else {
        box.continuation.finish(throwing: FocalError(status: status, message: String(cString: message!)))
    }
    box.finish()
}
