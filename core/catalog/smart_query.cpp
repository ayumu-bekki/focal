#include "catalog/smart_query.h"

#include <cctype>
#include <nlohmann/json.hpp>

#include "util/error.h"
#include "util/unicode.h"

namespace focal {

namespace {

using json = nlohmann::json;

[[noreturn]] void bad(const std::string& message) { throw Error(Error::Code::InvalidArgument, "smart album: " + message); }

std::string like_escape(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == '%' || c == '_' || c == '\\') r += '\\';
        r += c;
    }
    return r;
}

bool is_date(const std::string& s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (size_t i = 0; i < s.size(); ++i)
        if (i != 4 && i != 7 && !std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    return true;
}

SqlClause numeric(const std::string& column, const std::string& op, const json& value) {
    if (op != "=" && op != ">=" && op != "<=") bad("bad operator " + op);
    if (!value.is_number()) bad("value must be a number");
    SqlClause c;
    c.sql = column + " " + op + " ?";
    if (value.is_number_integer())
        c.args.emplace_back(value.get<int64_t>());
    else
        c.args.emplace_back(value.get<double>());
    return c;
}

SqlClause text_match(const std::string& expr, const std::string& op, const json& value) {
    if (op != "contains" && op != "not_contains") bad("bad operator " + op);
    if (!value.is_string() || value.get<std::string>().empty()) bad("value must be a non-empty string");
    SqlClause c;
    c.sql = std::string(op == "contains" ? "COALESCE(" : "NOT COALESCE(") + expr + " LIKE ? ESCAPE '\\', 0)";
    c.args.emplace_back("%" + like_escape(to_nfc(value.get<std::string>())) + "%");
    return c;
}

SqlClause id_membership(const std::string& op, const std::string& yes, const std::string& no, const json& value,
                        const std::string& subquery) {
    if (op != yes && op != no) bad("bad operator " + op);
    if (!value.is_number_integer()) bad("value must be an id");
    SqlClause c;
    c.sql = std::string("p.id ") + (op == yes ? "IN" : "NOT IN") + " (" + subquery + ")";
    c.args.emplace_back(value.get<int64_t>());
    return c;
}

SqlClause rule_clause(const json& r) {
    if (!r.is_object()) bad("rule must be an object");
    const std::string field = r.value("field", "");
    const std::string op = r.value("op", "");
    if (!r.contains("value")) bad("rule has no value");
    const json& value = r.at("value");

    if (field == "rating") {
        if (!value.is_number_integer() || value.get<int>() < 0 || value.get<int>() > 5) bad("rating must be 0-5");
        return numeric("p.rating", op, value);
    }
    if (field == "flag") {
        if (op != "is" && op != "is_not") bad("bad operator " + op);
        const std::string v = value.is_string() ? value.get<std::string>() : "";
        int f;
        if (v == "pick") f = 1;
        else if (v == "reject") f = -1;
        else if (v == "none") f = 0;
        else bad("flag must be pick, reject or none");
        SqlClause c;
        c.sql = std::string("p.flag ") + (op == "is" ? "=" : "<>") + " ?";
        c.args.emplace_back(static_cast<int64_t>(f));
        return c;
    }
    if (field == "tag")
        return id_membership(op, "has", "not_has", value,
                             "SELECT pt.photo_id FROM photo_tags pt WHERE pt.tag_id IN"
                             " (WITH RECURSIVE sub(id) AS (SELECT ? UNION ALL"
                             " SELECT c.id FROM tags c JOIN sub ON c.parent_id = sub.id) SELECT id FROM sub)");
    if (field == "album")
        return id_membership(op, "in", "not_in", value,
                             "SELECT ap.photo_id FROM album_photos ap WHERE ap.album_id IN"
                             " (WITH RECURSIVE sub(id) AS (SELECT ? UNION ALL"
                             " SELECT c.id FROM albums c JOIN sub ON c.parent_id = sub.id) SELECT id FROM sub)");
    if (field == "folder") {
        if (op != "in" && op != "not_in") bad("bad operator " + op);
        if (!value.is_number_integer()) bad("value must be an id");
        SqlClause c;
        c.sql = std::string("p.folder_id ") + (op == "in" ? "IN" : "NOT IN") +
                " (WITH RECURSIVE sub(id) AS (SELECT ? UNION ALL"
                " SELECT c.id FROM folders c JOIN sub ON c.parent_id = sub.id) SELECT id FROM sub)";
        c.args.emplace_back(value.get<int64_t>());
        return c;
    }
    if (field == "camera") return text_match("(COALESCE(p.camera_make, '') || ' ' || COALESCE(p.camera_model, ''))", op, value);
    if (field == "lens") return text_match("p.lens_model", op, value);
    if (field == "file_name") return text_match("p.file_name", op, value);
    if (field == "iso") return numeric("p.iso", op, value);
    if (field == "focal_length") return numeric("p.focal_length", op, value);
    if (field == "f_number") return numeric("p.f_number", op, value);
    if (field == "exposure_time") return numeric("p.exposure_time", op, value);
    if (field == "date") {
        SqlClause c;
        auto date = [&](const json& v) {
            if (!v.is_string() || !is_date(v.get<std::string>())) bad("date must be YYYY-MM-DD");
            return v.get<std::string>();
        };
        if (op == ">=") {
            c.sql = "p.capture_time >= ?";
            c.args.emplace_back(date(value));
        } else if (op == "<=") {
            c.sql = "p.capture_time <= ?";
            c.args.emplace_back(date(value) + "T23:59:59");
        } else if (op == "between") {
            if (!value.is_array() || value.size() != 2) bad("between needs two dates");
            c.sql = "(p.capture_time >= ? AND p.capture_time <= ?)";
            c.args.emplace_back(date(value[0]));
            c.args.emplace_back(date(value[1]) + "T23:59:59");
        } else {
            bad("bad operator " + op);
        }
        return c;
    }
    bad("unknown field " + field);
}

SqlClause build(const json& q) {
    if (!q.is_object()) bad("query must be an object");
    const std::string match = q.value("match", "all");
    if (match != "all" && match != "any") bad("match must be all or any");
    SqlClause out;
    if (!q.contains("rules") || q.at("rules").empty()) {
        out.sql = "1 = 1";
        return out;
    }
    if (!q.at("rules").is_array()) bad("rules must be an array");
    std::string joined;
    for (const auto& r : q.at("rules")) {
        SqlClause c = rule_clause(r);
        if (!joined.empty()) joined += match == "all" ? " AND " : " OR ";
        joined += "(" + c.sql + ")";
        for (auto& a : c.args) out.args.push_back(std::move(a));
    }
    out.sql = "(" + joined + ")";
    return out;
}

json parse(const std::string& text) {
    try {
        return json::parse(text);
    } catch (const json::exception& e) {
        bad(std::string("invalid JSON: ") + e.what());
    }
}

} // namespace

std::string empty_smart_query() { return R"({"match":"all","rules":[]})"; }

// 値の型が違うときの nlohmann の例外も InvalidArgument にそろえる
SqlClause build_checked(const std::string& text) {
    try {
        return build(parse(text));
    } catch (const json::exception& e) {
        bad(std::string("invalid query: ") + e.what());
    }
}

void validate_smart_query(const std::string& text) { (void)build_checked(text); }

SqlClause smart_query_clause(const std::string& text) {
    try {
        return build_checked(text);
    } catch (const Error&) {
        return {"0 = 1", {}};
    }
}

} // namespace focal
