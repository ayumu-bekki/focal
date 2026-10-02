import CFocal
import Foundation

/// サムネイルの要求キュー（fc_thumbnailer）。後から来た要求を先に処理する。
/// await 中の Task をキャンセルすると、開始前なら core 側の要求も取り消す（10 章: 画面外に出たセル）。
public final class Thumbnailer: @unchecked Sendable {
    let handle: OpaquePointer
    private let catalog: Catalog  // thumbnailer より長く生かす

    public init(catalog: Catalog, cacheDirectory: URL, threads: Int = 0) throws {
        var h: OpaquePointer?
        try cacheDirectory.path.withCString {
            try check(fc_thumbnailer_create(catalog.handle, $0, Int32(threads), &h))
        }
        handle = h!
        self.catalog = catalog
    }

    deinit { fc_thumbnailer_destroy(handle) }

    /// キャッシュの JPEG（sRGB、長辺 512px）の URL
    public func thumbnailURL(for photoID: Int64) async throws -> URL {
        let box = ThumbBox()
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (cont: CheckedContinuation<URL, Error>) in
                box.setContinuation(cont)
                let user = Unmanaged.passRetained(box).toOpaque()
                let rid = fc_thumbnailer_request(handle, photoID, thumbnailDone, user)
                if rid == 0 {
                    Unmanaged<ThumbBox>.fromOpaque(user).release()
                    box.resume(.failure(FocalError(status: FC_ERR_INVALID_ARGUMENT)))
                    return
                }
                if box.setRequestID(rid) { fc_thumbnailer_cancel(handle, rid) }
            }
        } onCancel: {
            if let rid = box.markCancelled() { fc_thumbnailer_cancel(handle, rid) }
        }
    }
}

private final class ThumbBox: @unchecked Sendable {
    private let lock = NSLock()
    private var continuation: CheckedContinuation<URL, Error>?
    private var requestID: UInt64 = 0
    private var cancelled = false

    func setContinuation(_ c: CheckedContinuation<URL, Error>) {
        lock.lock()
        continuation = c
        lock.unlock()
    }

    /// 要求 id を記録する。既にキャンセルされていれば true（呼び出し側でキャンセルする）
    func setRequestID(_ id: UInt64) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        requestID = id
        return cancelled
    }

    /// キャンセルを記録する。要求 id が分かっていればそれを返す
    func markCancelled() -> UInt64? {
        lock.lock()
        defer { lock.unlock() }
        cancelled = true
        return requestID == 0 ? nil : requestID
    }

    func resume(_ result: Result<URL, Error>) {
        lock.lock()
        let c = continuation
        continuation = nil
        lock.unlock()
        c?.resume(with: result)
    }
}

private func thumbnailDone(_ user: UnsafeMutableRawPointer?, _ requestID: UInt64, _ status: fc_status,
                           _ path: UnsafePointer<CChar>?) {
    let box = Unmanaged<ThumbBox>.fromOpaque(user!).takeRetainedValue()
    if status == FC_OK, let path {
        box.resume(.success(URL(fileURLWithPath: String(cString: path))))
    } else if status == FC_ERR_CANCELLED {
        box.resume(.failure(CancellationError()))
    } else {
        box.resume(.failure(FocalError(status: status, message: "thumbnail failed")))
    }
}
