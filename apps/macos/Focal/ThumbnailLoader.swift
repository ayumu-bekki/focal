import AppKit
import FocalCore
import ImageIO

/// サムネイルの読み込みとメモリキャッシュ（10 章）。
/// core の Thumbnailer でキャッシュの JPEG を用意し、ImageIO でデコードして sRGB をタグ付けする（8.1 章）。
final class ThumbnailLoader: @unchecked Sendable {
    private let thumbnailer: Thumbnailer
    private let cache = NSCache<NSNumber, CGImage>()
    private static let srgb = CGColorSpace(name: CGColorSpace.sRGB)!

    init(thumbnailer: Thumbnailer) {
        self.thumbnailer = thumbnailer
        cache.countLimit = 2000  // 512px × 2000 枚 ≒ 1.5GB を上限の目安に
    }

    func cached(_ id: Int64) -> CGImage? { cache.object(forKey: NSNumber(value: id)) }

    /// Task がキャンセルされると、開始前なら core 側の要求も取り消す
    func image(for id: Int64) async -> CGImage? {
        if let img = cached(id) { return img }
        guard let url = try? await thumbnailer.thumbnailURL(for: id), !Task.isCancelled else { return nil }
        guard let img = Self.decode(url) else { return nil }
        cache.setObject(img, forKey: NSNumber(value: id))
        return img
    }

    func invalidate(_ id: Int64) { cache.removeObject(forKey: NSNumber(value: id)) }

    /// バックグラウンドで完全にデコードした画像を作る。
    /// 遅延デコードの CGImage をレイヤーに渡すと、表示の確定時にメインスレッドで JPEG を展開してしまう（ヒッチの原因）
    private static func decode(_ url: URL) -> CGImage? {
        guard let src = CGImageSourceCreateWithURL(url as CFURL, nil),
              let img = CGImageSourceCreateImageAtIndex(src, 0, nil) else { return nil }
        // キャッシュの JPEG は ICC なしの sRGB という約束なので、sRGB のビットマップに描き込む（8.1 章）
        guard let ctx = CGContext(data: nil, width: img.width, height: img.height, bitsPerComponent: 8, bytesPerRow: 0,
                                  space: srgb,
                                  bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        else { return nil }
        let untagged = img.copy(colorSpace: srgb) ?? img
        ctx.draw(untagged, in: CGRect(x: 0, y: 0, width: img.width, height: img.height))
        return ctx.makeImage()
    }
}
