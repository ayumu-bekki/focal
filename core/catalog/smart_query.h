#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace focal {

// スマートアルバム（v3.19、design.md 7.2 章）の検索条件。JSON で albums.query に保存する。
//
//   {"match": "all" | "any", "rules": [{"field": …, "op": …, "value": …}, …]}
//
//   field            op                                value
//   rating           "=" ">=" "<="                     0〜5
//   flag             "is" "is_not"                     "pick" "reject" "none"
//   tag              "has" "not_has"                   タグの id（子孫のタグも含む）
//   album            "in" "not_in"                     アルバムの id（フォルダなら中のアルバムも含む）
//   folder           "in" "not_in"                     フォルダの id（サブフォルダも含む）
//   camera / lens    "contains" "not_contains"         文字列（カメラはメーカー + 機種）
//   file_name        "contains" "not_contains"         文字列
//   iso / focal_length / f_number / exposure_time
//                    "=" ">=" "<="                     数値
//   date             ">=" "<=" "between"               "YYYY-MM-DD"（between は 2 要素の配列）
//
// 条件に NOT と OR を持てるのがタグとの違い（スマートアルバムは読み取り専用なので、写真に何も付けない）。

using SqlArg = std::variant<int64_t, double, std::string>;

struct SqlClause {
    std::string sql;  // "(…)"。条件が空なら "1 = 1"
    std::vector<SqlArg> args;
};

// 条件を検証する。不正なら Error(InvalidArgument)
void validate_smart_query(const std::string& json);

// 条件を p（photos）に対する WHERE 句の断片にする。保存済みの JSON が壊れていたら何にも一致しない条件にする
SqlClause smart_query_clause(const std::string& json);

// 空の条件（すべての写真）
std::string empty_smart_query();

} // namespace focal
