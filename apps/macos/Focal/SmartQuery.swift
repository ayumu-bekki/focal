import Foundation
import SwiftUI

/// スマートアルバムの条件の編集用モデル（v3.19）。
/// 条件の意味づけ・検証は core（catalog/smart_query）が行う。ここは編集画面の行と JSON（design.md 7.2 章）の変換だけ
struct SmartRule: Identifiable, Equatable {
    enum Field: String, CaseIterable, Identifiable {
        case rating, flag, tag, album, camera, lens
        case fileName = "file_name"
        case iso
        case focalLength = "focal_length"
        case fNumber = "f_number"
        case date

        var id: String { rawValue }

        var title: LocalizedStringKey {
            switch self {
            case .rating: "Rating"
            case .flag: "Flag"
            case .tag: "Tag"
            case .album: "Album"
            case .camera: "Camera"
            case .lens: "Lens"
            case .fileName: "File Name"
            case .iso: "ISO"
            case .focalLength: "Focal Length"
            case .fNumber: "Aperture (f/)"
            case .date: "Capture Date"
            }
        }

        /// (JSON の op, 表示名)
        var operators: [(op: String, title: LocalizedStringKey)] {
            switch self {
            case .rating, .iso, .focalLength, .fNumber:
                [(">=", "is at least"), ("<=", "is at most"), ("=", "is")]
            case .flag: [("is", "is"), ("is_not", "is not")]
            case .tag: [("has", "has"), ("not_has", "does not have")]
            case .album: [("in", "is in"), ("not_in", "is not in")]
            case .camera, .lens, .fileName: [("contains", "contains"), ("not_contains", "does not contain")]
            case .date: [(">=", "is on or after"), ("<=", "is on or before"), ("between", "is between")]
            }
        }
    }

    let id = UUID()
    var field: Field = .rating
    var op = ">="
    var number = 3.0  // ★、ISO、焦点距離、F 値
    var text = ""  // カメラ、レンズ、ファイル名
    var flag = "pick"  // pick / reject / none
    var itemID: Int64 = 0  // タグ・アルバムの id
    var date = Date.now
    var dateEnd = Date.now

    init() {}

    /// 項目を変えたときの、その項目の最初の演算子と既定の値
    mutating func setField(_ f: Field) {
        field = f
        op = f.operators[0].op
        switch f {
        case .rating: number = 3
        case .iso: number = 1600
        case .focalLength: number = 50
        case .fNumber: number = 2.8
        default: break
        }
    }

    private static let dateFormatter: DateFormatter = {
        let f = DateFormatter()
        f.calendar = Calendar(identifier: .gregorian)
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy-MM-dd"
        return f
    }()

    /// JSON の 1 条件
    var json: [String: Any] {
        let value: Any
        switch field {
        case .rating: value = Int(number)
        case .iso: value = Int(number)
        case .focalLength, .fNumber: value = number
        case .flag: value = flag
        case .tag, .album: value = itemID
        case .camera, .lens, .fileName: value = text
        case .date:
            let a = Self.dateFormatter.string(from: date)
            value = op == "between" ? [a, Self.dateFormatter.string(from: dateEnd)] : a
        }
        return ["field": field.rawValue, "op": op, "value": value]
    }

    init?(json: [String: Any]) {
        guard let name = json["field"] as? String, let f = Field(rawValue: name), let op = json["op"] as? String,
              let value = json["value"] else { return nil }
        field = f
        self.op = op
        switch f {
        case .rating, .iso, .focalLength, .fNumber: number = (value as? NSNumber)?.doubleValue ?? 0
        case .flag: flag = value as? String ?? "pick"
        case .tag, .album: itemID = (value as? NSNumber)?.int64Value ?? 0
        case .camera, .lens, .fileName: text = value as? String ?? ""
        case .date:
            if let pair = value as? [String], pair.count == 2 {
                date = Self.dateFormatter.date(from: pair[0]) ?? .now
                dateEnd = Self.dateFormatter.date(from: pair[1]) ?? .now
            } else if let s = value as? String {
                date = Self.dateFormatter.date(from: s) ?? .now
            }
        }
    }
}

struct SmartQuery: Equatable {
    var matchAll = true
    var rules: [SmartRule] = []

    init() {}

    var jsonString: String {
        let object: [String: Any] = ["match": matchAll ? "all" : "any", "rules": rules.map(\.json)]
        let data = try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])
        return data.flatMap { String(data: $0, encoding: .utf8) } ?? #"{"match":"all","rules":[]}"#
    }

    init?(jsonString: String) {
        guard let data = jsonString.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        matchAll = (object["match"] as? String ?? "all") != "any"
        rules = (object["rules"] as? [[String: Any]] ?? []).compactMap(SmartRule.init(json:))
    }
}
