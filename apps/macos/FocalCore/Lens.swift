import CFocal
import Foundation

/// Lensfun のレンズ DB の 1 件（v3.27）
public struct LensInfo: Identifiable, Hashable, Sendable {
    /// "メーカー|モデル"（DevelopSettings.lensID）
    public let id: String
    public let maker: String
    public let model: String
    public let mounts: String
    public let cropFactor: Float
    public let minFocal, maxFocal: Float
    public let hasDistortion, hasTCA, hasVignetting: Bool

    /// 表示用の名前（モデルがメーカー名で始まらなければ付ける）
    public var displayName: String {
        model.lowercased().hasPrefix(maker.lowercased()) ? model : "\(maker) \(model)"
    }

    static func list(_ a: UnsafeMutablePointer<fc_lens_array>) -> [LensInfo] {
        (0..<a.pointee.count).map { i in
            let l = a.pointee.items[i]
            return LensInfo(id: String(cString: l.id), maker: String(cString: l.maker), model: String(cString: l.model),
                            mounts: String(cString: l.mounts), cropFactor: l.crop_factor, minFocal: l.min_focal,
                            maxFocal: l.max_focal, hasDistortion: l.has_distortion != 0, hasTCA: l.has_tca != 0,
                            hasVignetting: l.has_vignetting != 0)
        }
    }
}

/// レンズ補正の DB（アプリに同梱 + 利用者が足せるフォルダ）。画像処理は core が行う
public enum LensLibrary {
    /// Lensfun を使えるビルドか
    public static var isSupported: Bool { fc_lens_supported() != 0 }

    /// 最初に使う前に 1 回。bundled = アプリ同梱の DB、user = 利用者が XML を足せるフォルダ
    public static func configure(bundled: URL?, user: URL?) {
        let b = bundled?.path
        let u = user?.path
        _ = withOptionalCString(b) { bp in withOptionalCString(u) { up in fc_lens_configure(bp, up) } }
    }

    public static func counts() -> (cameras: Int, lenses: Int) {
        var c: Int32 = 0, l: Int32 = 0
        _ = fc_lens_counts(&c, &l)
        return (Int(c), Int(l))
    }

    public static func search(_ query: String, mount: String? = nil, limit: Int = 50) -> [LensInfo] {
        var a: UnsafeMutablePointer<fc_lens_array>?
        let st = withOptionalCString(mount) { m in query.withCString { fc_lens_search($0, m, Int32(limit), &a) } }
        guard st == FC_OK, let a else { return [] }
        defer { fc_lens_array_free(a) }
        return LensInfo.list(a)
    }

    public static func find(id: String) -> LensInfo? {
        var a: UnsafeMutablePointer<fc_lens_array>?
        guard id.withCString({ fc_lens_find($0, &a) }) == FC_OK, let a else { return nil }
        defer { fc_lens_array_free(a) }
        return LensInfo.list(a).first
    }
}
