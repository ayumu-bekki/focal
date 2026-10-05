import CFocal
import Foundation

/// 現像のプリセット 1 件（v3.20）。調整だけを持ち、切り取り・回転・傾き補正は含まない
public struct PresetInfo: Identifiable, Hashable, Sendable {
    /// "builtin:…"（アプリに同梱。消せない）または "user:…"
    public let id: String
    public let name: String
    public let isBuiltIn: Bool
}

/// プリセットの置き場（fc_presets）。1 つのプリセットは 1 つの JSON ファイル。C API はスレッド安全
public final class PresetStore: @unchecked Sendable {
    let handle: OpaquePointer

    /// userDirectory: 利用者のプリセット（読み書き）、builtInDirectory: アプリに同梱したもの（読み取り専用）
    public init(userDirectory: URL, builtInDirectory: URL? = nil) throws {
        var h: OpaquePointer?
        try userDirectory.path.withCString { user in
            try withOptionalCString(builtInDirectory?.path) { builtin in
                try check(fc_presets_open(user, builtin, &h))
            }
        }
        handle = h!
    }

    deinit { fc_presets_close(handle) }

    /// 同梱のものが先、それぞれ名前順
    public func list() throws -> [PresetInfo] {
        var a: UnsafeMutablePointer<fc_preset_array>?
        try check(fc_presets_list(handle, &a))
        defer { fc_preset_array_free(a) }
        return UnsafeBufferPointer(start: a!.pointee.items, count: a!.pointee.count).map {
            PresetInfo(id: String(cString: $0.id), name: String(cString: $0.name), isBuiltIn: $0.builtin != 0)
        }
    }

    /// 設定の調整をプリセットとして保存する（切り取りなどは保存されない）。同じ名前があれば上書き。保存した id を返す
    @discardableResult
    public func save(name: String, from settings: DevelopSettings) throws -> String {
        var s = settings.c
        var out: UnsafeMutablePointer<fc_string>?
        try name.withCString { try check(fc_presets_save(handle, $0, &s, &out)) }
        defer { fc_string_free(out) }
        return String(cString: out!.pointee.value)
    }

    /// 利用者のプリセットだけ消せる
    public func delete(id: String) throws {
        try id.withCString { try check(fc_presets_delete(handle, $0)) }
    }

    /// 利用者のプリセットの表示名を変える
    public func rename(id: String, to name: String) throws {
        try id.withCString { cid in try name.withCString { try check(fc_presets_rename(handle, cid, $0)) } }
    }

    /// settings の調整と同じ調整のプリセット（同梱が先。切り取りなどは見ない）。なければ nil。
    /// 読み込み済みの内容と比べるので、スライダーの操作のたびに呼んでよい
    public func matching(_ settings: DevelopSettings) -> String? {
        var c = settings.c
        var out: UnsafeMutablePointer<fc_string>?
        guard fc_presets_match(handle, &c, &out) == FC_OK, let out else { return nil }
        defer { fc_string_free(out) }
        return String(cString: out.pointee.value)
    }

    /// settings にプリセットの調整を重ねた結果（切り取り・回転・傾きは settings のまま）
    public func applying(id: String, to settings: DevelopSettings) throws -> DevelopSettings {
        var base = settings.c
        var out = fc_settings()
        try id.withCString { try check(fc_presets_apply(handle, $0, &base, &out)) }
        return DevelopSettings(out)
    }
}

extension Catalog {
    /// 写真のカタログ上の編集にプリセットを重ねる（書き込みは core のスレッドで、完了を待たない。切り取りは各写真のまま）。
    /// 現像ビューアで開いている写真には使わず、`PresetStore.applying` の結果をセッションに設定する
    public func applyPreset(id: String, from presets: PresetStore, to photoIDs: [Int64]) throws {
        try id.withCString { cid in
            try photoIDs.withUnsafeBufferPointer {
                try check(fc_catalog_apply_preset(handle, presets.handle, cid, $0.baseAddress, $0.count))
            }
        }
    }
}
