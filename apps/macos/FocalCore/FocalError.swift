import CFocal
import Foundation

/// C API のエラー（fc_status と fc_last_error のメッセージ）
public struct FocalError: Error, CustomStringConvertible, Sendable {
    public enum Code: Int32, Sendable {
        case invalidArgument = 1, io, unsupported, decode, internalError, cancelled, database, notFound
    }

    public let code: Code
    public let message: String

    public var description: String { "\(code): \(message)" }

    init(status: fc_status, message: String? = nil) {
        code = Code(rawValue: Int32(status.rawValue)) ?? .internalError
        self.message = message ?? String(cString: fc_last_error())
    }
}

/// fc_status を確認し、失敗なら FocalError を投げる（メッセージは同じスレッドの fc_last_error から取る）
@inline(__always)
func check(_ status: fc_status) throws {
    if status != FC_OK { throw FocalError(status: status) }
}

/// ヘッダとライブラリの API バージョンが一致するか（4.3 章）
public enum FocalCoreInfo {
    public static var apiVersion: Int32 { fc_api_version() }
    public static var isCompatible: Bool { fc_api_version() == FC_API_VERSION }
}

extension String {
    /// C 文字列（NULL 可）から。NULL なら nil
    init?(optionalCString p: UnsafePointer<CChar>?) {
        guard let p else { return nil }
        self.init(cString: p)
    }
}
