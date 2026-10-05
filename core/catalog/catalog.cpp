#include "catalog/catalog.h"

#include "edit/preset.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <map>
#include <set>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <variant>

#include "catalog/db_writer.h"
#include "catalog/smart_query.h"
#include "catalog/sqlite.h"
#include "imaging/libraw_util.h"
#include "imaging/photo_file.h"
#include "thumbs/thumbnail.h"
#include "util/error.h"
#include "util/file.h"
#include "util/hash.h"
#include "util/thread_pool.h"
#include "util/unicode.h"
#include "util/volume.h"

namespace focal {

namespace fs = std::filesystem;
using db::Database;
using db::Statement;

namespace {

constexpr int kScanChunk = 64;

// photos.imported_at と同じ書式の現在時刻（UTC）
std::string utc_now_iso() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string album_name(std::string_view name) {
    std::string n = to_nfc(name);
    const auto b = n.find_first_not_of(" \t"), e = n.find_last_not_of(" \t");
    n = b == std::string::npos ? std::string() : n.substr(b, e - b + 1);
    if (n.empty()) throw Error(Error::Code::InvalidArgument, "album name is empty");
    return n;
}

bool is_raw_file(const std::string& name) {
    const auto kind = photo_kind_for_name(name);
    return kind && *kind == PhotoKind::Raw;
}

std::string join_path(std::string_view root, std::string_view rel, std::string_view name = {}) {
    std::string s(root);
    if (!rel.empty()) {
        if (s.empty() || s.back() != '/') s += '/';
        s += rel;
    }
    if (!name.empty()) {
        if (s.empty() || s.back() != '/') s += '/';
        s += name;
    }
    return s;
}

std::string parent_rel(const std::string& rel) {
    const auto slash = rel.find_last_of('/');
    return slash == std::string::npos ? std::string() : rel.substr(0, slash);
}

std::optional<std::string> local_time_string(std::time_t t) {
    if (t <= 0) return std::nullopt;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    return std::string(buf);
}

template <class T>
std::optional<T> positive(T v) {
    return v > 0 ? std::optional<T>(v) : std::nullopt;
}

// ---- 走査 -------------------------------------------------------------

struct FoundFile {
    std::string folder_rel;  // NFC
    std::string name;        // NFC
    fs::path disk;
    FileStat st;
    PhotoKind kind = PhotoKind::Raw;
    std::vector<std::string> companions;  // 同じ名前の付属の写真ファイル（RAW の JPEG など）。NFC、名前順
};

std::string join_companions(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& n : names) out += (out.empty() ? "" : "/") + n;
    return out;
}

// 拡張子を除いた名前
std::string file_stem(const std::string& name) {
    const auto dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

// 他のアプリが管理するライブラリ（中身を触る必要がなく、macOS では「写真」ライブラリの中へ入ると
// 「写真」へのアクセスの許可を求められる）。スキャンは中へ入らず、フォルダとしても登録しない（v3.19）
bool is_managed_library(const std::string& name) {
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == "photoslibrary" || ext == "aplibrary" || ext == "migratedphotolibrary" ||
           ext == "migratedaplibrary" || ext == "lrdata";
}

void walk(const fs::path& disk_root, std::vector<std::string>& dirs, std::vector<FoundFile>& files) {
    dirs.push_back("");
    std::error_code ec;
    fs::recursive_directory_iterator it(disk_root, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) throw Error(Error::Code::Io, "cannot read " + path_to_utf8(disk_root) + ": " + ec.message());
    for (; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const std::string name = to_nfc(path_to_utf8(it->path().filename()));
        if (!name.empty() && name[0] == '.') {  // 隠しファイル・フォルダ（.Trashes など）は飛ばす
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        const auto rel_u8 = it->path().lexically_relative(disk_root).generic_u8string();
        const std::string rel = to_nfc(std::string(reinterpret_cast<const char*>(rel_u8.data()), rel_u8.size()));
        if (it->is_directory(ec)) {
            if (is_managed_library(name)) {
                it.disable_recursion_pending();
                continue;
            }
            if (it->is_symlink(ec)) {
                it.disable_recursion_pending();  // シンボリックリンクのフォルダはたどらない（循環を避ける）
                continue;
            }
            dirs.push_back(rel);
        } else if (const auto kind = photo_kind_for_name(name)) {
            if (auto st = stat_file(it->path())) files.push_back({parent_rel(rel), name, it->path(), *st, *kind, {}});
        }
    }
    std::sort(dirs.begin(), dirs.end());  // 親が子より先に来る
}

// 同じフォルダで、拡張子を除いた名前が同じ（大文字小文字を区別しない）写真ファイルを 1 枚にする（v3.22、5.12 章）。
// RAW があれば、同じ名前の RAW 以外（JPEG・TIFF・PNG・HEIF）はその RAW（名前順で先頭）の付属ファイルになり、
// 写真の行を持たない。RAW がなければ、それぞれが別の写真。RAW 同士は別の写真（グループにしない）
void attach_companions(std::vector<FoundFile>& files) {
    std::map<std::pair<std::string, std::string>, std::vector<size_t>> groups;
    for (size_t i = 0; i < files.size(); ++i)
        groups[{files[i].folder_rel, casefold_key(file_stem(files[i].name))}].push_back(i);
    std::vector<bool> drop(files.size(), false);
    for (auto& [key, idx] : groups) {
        if (idx.size() < 2) continue;
        std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return files[a].name < files[b].name; });
        const auto raw = std::find_if(idx.begin(), idx.end(), [&](size_t i) { return files[i].kind == PhotoKind::Raw; });
        if (raw == idx.end()) continue;
        for (size_t i : idx) {
            if (files[i].kind == PhotoKind::Raw) continue;
            files[*raw].companions.push_back(files[i].name);
            drop[i] = true;
        }
    }
    size_t out = 0;
    for (size_t i = 0; i < files.size(); ++i)
        if (!drop[i]) {
            if (out != i) files[out] = std::move(files[i]);  // 自分自身へのムーブは中身が空になる
            ++out;
        }
    files.resize(out);
}

// ---- 1 ファイル分の読み取り（サムネイルプールで並列に実行） ------------

struct Work {
    int64_t folder_id = 0;
    std::string name;
    std::string nfc_path;
    fs::path disk;
    FileStat st;
    std::optional<int64_t> photo_id;  // 既存の写真を読み直すとき
    bool was_missing = false;
    bool thumb_only = false;  // メタデータは変わっていないが、サムネイルがない
    PhotoKind kind = PhotoKind::Raw;
    std::string companions;  // join_companions
};

struct Probe {
    bool readable = false;
    RawMetadata meta;
    std::string hash;
    bool thumb_made = false;
    bool thumb_failed = false;
};

Probe probe(const Work& w, const ThumbnailCache* thumbs) {
    Probe p;
    if (w.kind != PhotoKind::Raw) {
        // RAW 以外の写真（v3.22）: 画像そのものを読んで、撮影情報とサムネイルを作る
        try {
            p.meta = read_image_file_metadata(w.disk, w.kind);
            p.readable = true;
        } catch (const std::exception&) {
            // 非対応・破損（status 2）
        }
        if (!w.thumb_only) {
            try {
                p.hash = quick_hash(w.disk);
            } catch (const Error&) {
            }
        }
        if (thumbs && p.readable) {
            const std::string key = thumbnail_key(w.nfc_path, w.st.size, w.st.mtime);
            if (!thumbs->contains(key)) {
                try {
                    thumbs->store(key, image_file_thumbnail(w.disk, w.kind, kThumbnailLongEdge));
                    p.thumb_made = true;
                } catch (const std::exception&) {
                    p.thumb_failed = true;
                }
            }
        }
        return p;
    }
    auto raw = std::make_unique<LibRaw>();
    try {
        open_libraw(*raw, w.disk);
        p.meta = metadata_from_libraw(*raw);
        p.readable = true;
    } catch (const Error&) {
        // 非対応・破損（status 2）
    }
    if (!w.thumb_only) {
        try {
            p.hash = quick_hash(w.disk);
        } catch (const Error&) {
        }
    }
    if (thumbs && p.readable) {
        const std::string key = thumbnail_key(w.nfc_path, w.st.size, w.st.mtime);
        if (!thumbs->contains(key)) {
            try {
                thumbs->store(key, make_thumbnail(*raw, w.disk).image);
                p.thumb_made = true;
            } catch (const std::exception&) {
                p.thumb_failed = true;
            }
        }
    }
    return p;
}

// 新しく見つかったファイルが、ほかの場所から移動してきた写真か（v3.19）。
// ファイルなしになっている写真のうち、ファイル名・サイズ・撮影日時が同じものがちょうど 1 枚ならそれ。
// ★・フラグ・タグ・アルバム・編集は写真の id に付いているので、つなぎ直せばすべて引き継がれる
std::optional<int64_t> find_relink_candidate(Database& db, const Work& w, const Probe& p) {
    if (!p.readable) return std::nullopt;
    const auto capture = local_time_string(p.meta.timestamp);
    if (!capture) return std::nullopt;
    auto st = db.prepare(
        "SELECT id FROM photos WHERE status = 1 AND file_name = ? COLLATE NOCASE AND file_size = ?"
        " AND capture_time = ? LIMIT 2");
    st.bind(1, w.name).bind(2, w.st.size).bind(3, *capture);
    std::optional<int64_t> found;
    while (st.step()) {
        if (found) return std::nullopt;  // 候補が複数ならどれか決められない（新規として足す）
        found = st.column_int64(0);
    }
    return found;
}

void write_probe(Database& db, const Work& w, const Probe& p) {
    const int status = p.readable ? static_cast<int>(PhotoStatus::Ok) : static_cast<int>(PhotoStatus::Unsupported);
    const auto& m = p.meta;
    const bool has_meta = p.readable;
    auto bind_meta = [&](Statement& st, int first) {
        int i = first;
        st.bind(i++, has_meta ? local_time_string(m.timestamp) : std::nullopt);
        st.bind(i++, has_meta && !m.make.empty() ? std::optional<std::string>(m.make) : std::nullopt);
        st.bind(i++, has_meta && !m.model.empty() ? std::optional<std::string>(m.model) : std::nullopt);
        st.bind(i++, has_meta && !m.lens.empty() ? std::optional<std::string>(m.lens) : std::nullopt);
        st.bind(i++, has_meta ? positive<int64_t>(static_cast<int64_t>(m.iso)) : std::nullopt);
        st.bind(i++, has_meta ? positive<double>(m.shutter) : std::nullopt);
        st.bind(i++, has_meta ? positive<double>(m.aperture) : std::nullopt);
        st.bind(i++, has_meta ? positive<double>(m.focal_length) : std::nullopt);
        st.bind(i++, has_meta ? positive<int64_t>(m.width) : std::nullopt);
        st.bind(i++, has_meta ? positive<int64_t>(m.height) : std::nullopt);
        st.bind(i++, static_cast<int64_t>(has_meta ? m.flip : 0));
        return i;
    };
    auto hash = p.hash.empty() ? std::optional<std::string>() : std::optional<std::string>(p.hash);
    const std::optional<std::string> companions =
        w.companions.empty() ? std::nullopt : std::optional<std::string>(w.companions);

    if (w.photo_id) {
        auto st = db.prepare(
            "UPDATE photos SET folder_id = ?, file_name = ?, file_size = ?, file_mtime = ?, quick_hash = ?, status = ?,"
            " capture_time = ?, camera_make = ?, camera_model = ?, lens_model = ?, iso = ?, exposure_time = ?,"
            " f_number = ?, focal_length = ?, width = ?, height = ?, orientation = ?, kind = ?, companions = ?"
            " WHERE id = ?");
        st.bind(1, w.folder_id).bind(2, w.name).bind(3, w.st.size).bind(4, w.st.mtime).bind(5, hash).bind(6, status);
        int next = bind_meta(st, 7);
        st.bind(next++, static_cast<int64_t>(w.kind));
        st.bind(next++, companions);
        st.bind(next, *w.photo_id);
        st.run();
    } else {
        auto st = db.prepare(
            "INSERT INTO photos (folder_id, file_name, file_size, file_mtime, quick_hash, status, capture_time,"
            " camera_make, camera_model, lens_model, iso, exposure_time, f_number, focal_length, width, height,"
            " orientation, kind, companions) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        st.bind(1, w.folder_id).bind(2, w.name).bind(3, w.st.size).bind(4, w.st.mtime).bind(5, hash).bind(6, status);
        int next = bind_meta(st, 7);
        st.bind(next++, static_cast<int64_t>(w.kind));
        st.bind(next, companions);
        st.run();
    }
}

// ---- クエリ ------------------------------------------------------------

constexpr const char* kPhotoColumns =
    "p.id, p.folder_id, p.file_name, p.file_size, p.file_mtime, p.quick_hash, p.status, p.capture_time,"
    " p.camera_make, p.camera_model, p.lens_model, p.iso, p.exposure_time, p.f_number, p.focal_length,"
    " p.width, p.height, p.orientation, p.rating, p.flag, r.path, f.rel_path, p.kind, p.companions";

// photos.companions（ファイル名を '/' で区切る）→ 並び
std::vector<std::string> split_companions(const std::optional<std::string>& text) {
    std::vector<std::string> out;
    if (!text) return out;
    size_t start = 0;
    while (start <= text->size()) {
        const size_t slash = text->find('/', start);
        const size_t end = slash == std::string::npos ? text->size() : slash;
        if (end > start) out.push_back(text->substr(start, end - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return out;
}

PhotoRecord read_photo(const Statement& st) {
    PhotoRecord r;
    r.id = st.column_int64(0);
    r.folder_id = st.column_int64(1);
    r.file_name = st.column_text(2);
    r.file_size = st.column_int64(3);
    r.file_mtime = st.column_int64(4);
    r.quick_hash = st.column_text(5);
    r.status = static_cast<PhotoStatus>(st.column_int(6));
    r.capture_time = st.column_opt_text(7);
    r.camera_make = st.column_text(8);
    r.camera_model = st.column_text(9);
    r.lens_model = st.column_text(10);
    r.iso = st.column_opt_int64(11);
    r.exposure_time = st.column_opt_double(12);
    r.f_number = st.column_opt_double(13);
    r.focal_length = st.column_opt_double(14);
    r.width = st.column_int(15);
    r.height = st.column_int(16);
    r.orientation = st.column_int(17);
    r.rating = st.column_int(18);
    r.flag = st.column_int(19);
    r.path = join_path(st.column_text(20), st.column_text(21), r.file_name);
    r.kind = static_cast<PhotoKind>(st.column_int(22));
    r.companions = split_companions(st.column_opt_text(23));
    return r;
}

// WHERE 句とバインドする値を組み立てる
struct WhereClause {
    std::string sql;
    std::vector<SqlArg> args;

    void append(const SqlClause& c) {
        sql += " AND " + c.sql;
        for (const auto& a : c.args) args.push_back(a);
    }

    void bind_all(Statement& st, int first = 1) const {
        int i = first;
        for (const auto& a : args) {
            if (std::holds_alternative<int64_t>(a))
                st.bind(i++, std::get<int64_t>(a));
            else if (std::holds_alternative<double>(a))
                st.bind(i++, std::get<double>(a));
            else
                st.bind(i++, std::get<std::string>(a));
        }
    }
};

// スマートアルバムの条件（albums.query）。スマートアルバムでなければ nullopt
std::optional<std::string> load_smart_query(Database& db, int64_t album_id) {
    auto st = db.prepare("SELECT query FROM albums WHERE id = ? AND kind = 2");
    st.bind(1, album_id);
    if (st.step()) return st.column_opt_text(0);
    return std::nullopt;
}

WhereClause build_where(const PhotoFilter& f, Database& db) {
    WhereClause w;
    w.sql = " WHERE 1 = 1";
    if (f.folder_id) {
        if (f.include_subfolders) {
            w.sql += " AND p.folder_id IN (WITH RECURSIVE sub(id) AS (SELECT ? UNION ALL"
                     " SELECT c.id FROM folders c JOIN sub ON c.parent_id = sub.id) SELECT id FROM sub)";
        } else {
            w.sql += " AND p.folder_id = ?";
        }
        w.args.emplace_back(*f.folder_id);
    }
    if (f.min_rating > 0) {
        w.sql += " AND p.rating >= ?";
        w.args.emplace_back(static_cast<int64_t>(f.min_rating));
    }
    switch (f.flag) {
    case FlagFilter::Any: break;
    case FlagFilter::Picked: w.sql += " AND p.flag = 1"; break;
    case FlagFilter::Rejected: w.sql += " AND p.flag = -1"; break;
    case FlagFilter::Unflagged: w.sql += " AND p.flag = 0"; break;
    case FlagFilter::NotRejected: w.sql += " AND p.flag >= 0"; break;
    }
    if (f.tag_id) {
        w.sql += " AND p.id IN (SELECT pt.photo_id FROM photo_tags pt WHERE pt.tag_id IN"
                 " (WITH RECURSIVE sub(id) AS (SELECT ? UNION ALL"
                 " SELECT c.id FROM tags c JOIN sub ON c.parent_id = sub.id) SELECT id FROM sub))";
        w.args.emplace_back(*f.tag_id);
    }
    if (!f.date_from.empty()) {
        w.sql += " AND p.capture_time >= ?";
        w.args.emplace_back(f.date_from);
    }
    if (!f.date_to.empty()) {
        w.sql += " AND p.capture_time <= ?";
        w.args.emplace_back(f.date_to + "T23:59:59");
    }
    if (!f.include_unavailable) w.sql += " AND p.status = 0";
    if (f.album_id) {
        w.sql += " AND p.id IN (SELECT ap.photo_id FROM album_photos ap WHERE ap.album_id = ?)";
        w.args.emplace_back(*f.album_id);
    }
    if (f.smart_album_id) {
        const auto q = load_smart_query(db, *f.smart_album_id);
        w.append(q ? smart_query_clause(*q) : SqlClause{"0 = 1", {}});
    }
    // 最後に写真を足した取り込みの開始時刻より後に足された写真（記録がなければ 0 枚）
    if (f.recent_import) w.sql += " AND p.imported_at >= (SELECT value FROM meta WHERE key = 'last_import_at')";
    return w;
}

std::vector<std::string> split_tag_path(std::string_view path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const std::string_view part = path.substr(start, slash == std::string_view::npos ? std::string_view::npos
                                                                                          : slash - start);
        if (!part.empty()) parts.push_back(to_nfc(part));
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    if (parts.empty()) throw Error(Error::Code::InvalidArgument, "empty tag path");
    return parts;
}

std::optional<int64_t> find_tag_child(Database& db, std::optional<int64_t> parent, const std::string& name) {
    auto st = parent ? db.prepare("SELECT id FROM tags WHERE parent_id = ? AND name = ?")
                     : db.prepare("SELECT id FROM tags WHERE parent_id IS NULL AND name = ?");
    int i = 1;
    if (parent) st.bind(i++, *parent);
    st.bind(i, name);
    if (st.step()) return st.column_int64(0);
    return std::nullopt;
}

constexpr const char* kTagTreeSql =
    "WITH RECURSIVE tree(id, parent_id, name, path) AS ("
    " SELECT id, parent_id, name, name FROM tags WHERE parent_id IS NULL"
    " UNION ALL SELECT t.id, t.parent_id, t.name, tree.path || '/' || t.name FROM tags t"
    " JOIN tree ON t.parent_id = tree.id)"
    " SELECT tree.id, tree.parent_id, tree.name, tree.path,"
    " (SELECT COUNT(*) FROM photo_tags pt WHERE pt.tag_id = tree.id) FROM tree";

TagInfo read_tag(const Statement& st) {
    return {st.column_int64(0), st.column_opt_int64(1), st.column_text(2), st.column_text(3), st.column_int64(4)};
}

} // namespace

// 新しく行を作った写真のうち、同じ内容のファイル（quick_hash とサイズが同じ）が別の場所に**残っている**ものは、
// そのコピーとして、元の写真の現像・★・フラグ・タグを引き継ぐ（v3.19）。引き継ぐのは登録のときだけで、
// 以後はそれぞれ独立。元のファイルがなくなっているときは、コピーではなく移動なので、ここでは何もしない
// （移動は find_relink_candidate が、同じ行につなぎ直す）。アルバムの所属は引き継がない。引き継いだ写真の数を返す
struct FreshPhoto {
    int64_t id;
    std::string hash;
    int64_t size;
};

int inherit_from_copies(db::DbWriter& writer, const std::vector<FreshPhoto>& fresh) {
    int inherited = 0;
    for (const auto& f : fresh) {
        struct Candidate {
            int64_t id;
            std::string abs_path;
        };
        // 情報の多い写真を先に（現像 > ★・フラグ > タグ）
        const auto cands = writer.call([&](Database& db) {
            std::vector<Candidate> out;
            auto st = db.prepare(
                "SELECT p.id, r.path, fo.rel_path, p.file_name FROM photos p"
                " JOIN folders fo ON fo.id = p.folder_id JOIN roots r ON r.id = fo.root_id"
                " WHERE p.quick_hash = ? AND p.file_size = ? AND p.id <> ?"
                " ORDER BY (EXISTS (SELECT 1 FROM edits e WHERE e.photo_id = p.id)) DESC,"
                " (p.rating > 0 OR p.flag <> 0) DESC, (EXISTS (SELECT 1 FROM photo_tags t WHERE t.photo_id = p.id)) DESC, p.id"
                " LIMIT 8");
            st.bind(1, f.hash).bind(2, f.size).bind(3, f.id);
            while (st.step()) out.push_back({st.column_int64(0), join_path(st.column_text(1), st.column_text(2), st.column_text(3))});
            return out;
        });
        std::optional<int64_t> source;
        for (const auto& c : cands) {
            const auto disk = resolve_nfc_path(c.abs_path);  // ディスクの確認は書き込みスレッドの外で
            std::error_code ec;
            if (disk && fs::exists(*disk, ec)) {
                source = c.id;
                break;
            }
        }
        if (!source) continue;
        const bool did = writer.call([&](Database& db) {
            auto has = db.prepare(
                "SELECT (SELECT COUNT(*) FROM edits WHERE photo_id = ?1) + (SELECT COUNT(*) FROM photo_tags WHERE photo_id = ?1)"
                " + (SELECT rating <> 0 OR flag <> 0 FROM photos WHERE id = ?1)");
            has.bind(1, *source);
            has.step();
            if (has.column_int(0) == 0) return false;  // 引き継ぐ情報がない
            db.prepare("INSERT OR IGNORE INTO edits (photo_id, process_version, settings, updated_at)"
                       " SELECT ?1, process_version, settings, updated_at FROM edits WHERE photo_id = ?2")
                .bind(1, f.id).bind(2, *source).run();
            db.prepare("UPDATE photos SET rating = (SELECT rating FROM photos WHERE id = ?2),"
                       " flag = (SELECT flag FROM photos WHERE id = ?2) WHERE id = ?1")
                .bind(1, f.id).bind(2, *source).run();
            db.prepare("INSERT OR IGNORE INTO photo_tags (photo_id, tag_id) SELECT ?1, tag_id FROM photo_tags WHERE photo_id = ?2")
                .bind(1, f.id).bind(2, *source).run();
            return true;
        });
        if (did) ++inherited;
    }
    return inherited;
}

// 新規・変更されたファイルを並列に読み、チャンクごとに書き込む（scan_root と register_files で共通）
void process_works(db::DbWriter& writer, const std::vector<Work>& work, const ScanOptions& opt, ScanStats& stats) {
    const unsigned threads = opt.threads ? opt.threads : std::max(1u, std::thread::hardware_concurrency() - 1);
    ThreadPool pool(threads);
    const int total = static_cast<int>(work.size());
    for (int start = 0; start < total; start += kScanChunk) {
        if (opt.cancel && opt.cancel->load()) throw Error(Error::Code::Cancelled, "scan cancelled");
        const int end = std::min(total, start + kScanChunk);
        std::vector<Probe> probes(end - start);
        pool.parallel_for(end - start, 1, [&](int b, int e) {
            for (int i = b; i < e; ++i) probes[i] = probe(work[start + i], opt.thumbnails);
        });

        for (int i = start; i < end; ++i) {
            const Work& w = work[i];
            const Probe& p = probes[i - start];
            stats.thumbnails += p.thumb_made;
            stats.thumbnail_failures += p.thumb_failed;
            if (w.thumb_only) continue;
            if (!p.readable) ++stats.unsupported;
            if (!w.photo_id)
                ++stats.added;
            else if (w.was_missing)
                ++stats.restored;
            else
                ++stats.updated;
        }
        std::vector<FreshPhoto> fresh;  // このチャンクで新しく行を作った写真（コピーの検出の対象）
        const int relinked = writer.call([&](Database& db) {
            int n = 0;
            for (int i = start; i < end; ++i) {
                if (work[i].thumb_only) continue;
                Work w = work[i];
                const Probe& p = probes[i - start];
                const bool is_new = !w.photo_id;
                if (!w.photo_id) {
                    if (const auto id = find_relink_candidate(db, w, p)) {
                        w.photo_id = id;
                        ++n;
                    }
                }
                write_probe(db, w, p);
                if (is_new && !w.photo_id && p.readable && !p.hash.empty())
                    fresh.push_back({db.last_insert_rowid(), p.hash, w.st.size});
            }
            return n;
        });
        stats.added -= relinked;
        stats.relinked += relinked;
        stats.inherited += inherit_from_copies(writer, fresh);
        if (opt.progress) opt.progress(end, total);
    }
}

// ---- Catalog -----------------------------------------------------------

std::unique_ptr<Catalog> Catalog::open(const fs::path& path) { return std::unique_ptr<Catalog>(new Catalog(path)); }

Catalog::Catalog(const fs::path& path) : path_(path) {
    if (path.has_parent_path()) fs::create_directories(path.parent_path());
    writer_ = std::make_unique<db::DbWriter>(path);  // スキーマを作ってから読み取り接続を開く
    reader_ = std::make_unique<Database>(path, Database::Mode::ReadOnly);
    // 外付けドライブのマウントポイントが前回と変わっていたら、ルートの場所を合わせる（v3.19）
    try {
        refresh_volumes();
    } catch (const Error&) {
    }
}

Catalog::~Catalog() = default;

fs::path Catalog::migration_backup() const { return writer_->backup_path(); }

namespace {

struct VolumeColumns {
    std::optional<std::string> id, name, rel;
};

// dir があるボリュームの情報（ボリュームを判別できなければ空）
VolumeColumns volume_columns(const fs::path& dir) {
    VolumeColumns v;
    const auto vol = volume_for_path(dir);
    if (!vol || vol->id.empty()) return v;
    const auto rel = volume_relative_path(*vol, dir);
    if (!rel) return v;
    v.id = vol->id;
    v.name = vol->name.empty() ? std::nullopt : std::optional<std::string>(to_nfc(vol->name));
    v.rel = *rel;
    return v;
}

} // namespace

namespace {

std::string local_timestamp_for_filename() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

// child が parent の中（parent 自身は含まない）にあるか。macOS・Windows は大文字小文字を区別しないので、折りたたんで比べる
bool path_inside(const std::string& parent, const std::string& child) {
    std::string a = casefold_key(parent), b = casefold_key(child);
    while (a.size() > 1 && a.back() == '/') a.pop_back();
    if (b.size() <= a.size()) return false;
    return a == "/" ? b.rfind("/", 0) == 0 : (b.compare(0, a.size(), a) == 0 && b[a.size()] == '/');
}

std::vector<std::pair<int64_t, std::string>> all_root_paths(Database& db) {
    std::vector<std::pair<int64_t, std::string>> out;
    auto st = db.prepare("SELECT id, path FROM roots ORDER BY id");
    while (st.step()) out.emplace_back(st.column_int64(0), st.column_text(1));
    return out;
}

// 写真の行に付いている情報の多さ（同じファイルの行が 2 つあるとき、どちらを残すかに使う）
int photo_richness(Database& db, int64_t id) {
    auto st = db.prepare(
        "SELECT (SELECT COUNT(*) FROM edits WHERE photo_id = ?1) * 4 + (rating > 0) * 2 + (flag <> 0) * 2"
        " + (SELECT COUNT(*) > 0 FROM photo_tags WHERE photo_id = ?1)"
        " + (SELECT COUNT(*) > 0 FROM album_photos WHERE photo_id = ?1) FROM photos WHERE id = ?1");
    st.bind(1, id);
    return st.step() ? st.column_int(0) : 0;
}

// 同じファイルの行 a・b を 1 つにする。情報の多い方を残し、残る方が持っていないものを、消す方から移す。
// 残った行の id を返す
// drop の行の情報を keep の行へ移して、drop を消す（keep が持っていないものだけ移す）
void absorb_photo_row(Database& db, int64_t keep, int64_t drop) {
    // 現像: 残す方になければ移す
    db.prepare("UPDATE edits SET photo_id = ?1 WHERE photo_id = ?2 AND NOT EXISTS (SELECT 1 FROM edits WHERE photo_id = ?1)")
        .bind(1, keep).bind(2, drop).run();
    // ★・フラグ: 残す方が空なら、消す方の値
    db.prepare("UPDATE photos SET rating = (SELECT rating FROM photos WHERE id = ?2) WHERE id = ?1 AND rating = 0")
        .bind(1, keep).bind(2, drop).run();
    db.prepare("UPDATE photos SET flag = (SELECT flag FROM photos WHERE id = ?2) WHERE id = ?1 AND flag = 0")
        .bind(1, keep).bind(2, drop).run();
    db.prepare("INSERT OR IGNORE INTO photo_tags (photo_id, tag_id) SELECT ?1, tag_id FROM photo_tags WHERE photo_id = ?2")
        .bind(1, keep).bind(2, drop).run();
    db.prepare("INSERT OR IGNORE INTO album_photos (album_id, photo_id, added_at)"
               " SELECT album_id, ?1, added_at FROM album_photos WHERE photo_id = ?2")
        .bind(1, keep).bind(2, drop).run();
    db.prepare("UPDATE albums SET cover_photo_id = ?1 WHERE cover_photo_id = ?2").bind(1, keep).bind(2, drop).run();
    db.prepare("DELETE FROM photos WHERE id = ?").bind(1, drop).run();
}

int64_t merge_photo_rows(Database& db, int64_t a, int64_t b) {
    const int ra = photo_richness(db, a), rb = photo_richness(db, b);
    const int64_t keep = (rb > ra || (rb == ra && b < a)) ? b : a;
    absorb_photo_row(db, keep, keep == a ? b : a);
    return keep;
}

// child のルートを parent のルートの下へ統合する（child_path は parent_path の中）。写真の行は移すだけで、
// id・現像・★・タグ・アルバムの所属はそのまま。parent にすでに同じファイルの行があれば merge_photo_rows で合わせる
void merge_root_into(Database& db, int64_t parent_id, const std::string& parent_path, int64_t child_id,
                     const std::string& child_path) {
    const size_t skip = parent_path == "/" ? 1 : parent_path.size() + 1;
    const std::string prefix = child_path.size() > skip ? child_path.substr(skip) : std::string();

    std::map<std::string, int64_t> parent_folders;
    {
        auto st = db.prepare("SELECT id, rel_path FROM folders WHERE root_id = ?");
        st.bind(1, parent_id);
        while (st.step()) parent_folders[st.column_text(1)] = st.column_int64(0);
    }
    auto ensure = [&](auto&& self, const std::string& rel) -> int64_t {
        if (auto it = parent_folders.find(rel); it != parent_folders.end()) return it->second;
        const std::optional<int64_t> up = rel.empty() ? std::nullopt : std::optional<int64_t>(self(self, parent_rel(rel)));
        db.prepare("INSERT INTO folders (root_id, parent_id, rel_path) VALUES (?, ?, ?)").bind(1, parent_id).bind(2, up).bind(3, rel).run();
        return parent_folders[rel] = db.last_insert_rowid();
    };

    std::vector<std::pair<int64_t, std::string>> child_folders;  // 親が先（rel_path の昇順）
    {
        auto st = db.prepare("SELECT id, rel_path FROM folders WHERE root_id = ? ORDER BY rel_path");
        st.bind(1, child_id);
        while (st.step()) child_folders.emplace_back(st.column_int64(0), st.column_text(1));
    }
    for (const auto& [cf, rel] : child_folders) {
        const std::string target_rel = rel.empty() ? prefix : (prefix.empty() ? rel : prefix + "/" + rel);
        const int64_t target = ensure(ensure, target_rel);
        std::vector<std::pair<int64_t, std::string>> photos;
        {
            auto st = db.prepare("SELECT id, file_name FROM photos WHERE folder_id = ?");
            st.bind(1, cf);
            while (st.step()) photos.emplace_back(st.column_int64(0), st.column_text(1));
        }
        for (const auto& [pid, name] : photos) {
            auto dup = db.prepare("SELECT id FROM photos WHERE folder_id = ? AND file_name = ?");
            dup.bind(1, target).bind(2, name);
            if (dup.step()) {
                const int64_t other = dup.column_int64(0);
                const int64_t keep = merge_photo_rows(db, other, pid);
                db.prepare("UPDATE photos SET folder_id = ? WHERE id = ?").bind(1, target).bind(2, keep).run();
            } else {
                db.prepare("UPDATE photos SET folder_id = ? WHERE id = ?").bind(1, target).bind(2, pid).run();
            }
        }
    }
    // 写真が 1 枚も残っていないことを確かめてから、ルート（と空のフォルダ）を消す。残っていれば、何も消さずに止める
    auto left = db.prepare("SELECT COUNT(*) FROM photos p JOIN folders f ON f.id = p.folder_id WHERE f.root_id = ?");
    left.bind(1, child_id);
    left.step();
    if (left.column_int64(0) != 0) throw Error(Error::Code::Internal, "root merge left photos behind; nothing was removed");
    db.prepare("DELETE FROM roots WHERE id = ?").bind(1, child_id).run();
}

} // namespace

int64_t Catalog::add_root(const fs::path& dir, std::string_view label) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) throw Error(Error::Code::NotFound, "not a directory: " + path_to_utf8(dir));
    const std::string p = normalized_path_string(dir);
    const std::string l = to_nfc(label);
    const VolumeColumns vol = volume_columns(dir);

    // 登録済みのルートの中のフォルダなら、新しいルートは作らず、そのルートを返す
    for (const auto& [id, path] : [&] {
             std::lock_guard lock(reader_mutex_);
             return all_root_paths(*reader_);
         }())
        if (path_inside(path, p)) return id;
    // 登録済みのルートを含むなら、統合の前にバックアップを作る
    bool contains_existing = false;
    {
        std::lock_guard lock(reader_mutex_);
        for (const auto& [id, path] : all_root_paths(*reader_))
            if (path_inside(p, path)) contains_existing = true;
    }
    if (contains_existing) backup("merge-roots");

    return writer_->call([&](Database& db) -> int64_t {
        auto find = db.prepare("SELECT id, volume_id FROM roots WHERE path = ?");
        find.bind(1, p);
        if (find.step()) {
            const int64_t id = find.column_int64(0);
            if (find.column_is_null(1) && vol.id) {  // v2 までのカタログで登録したルートにボリュームの情報を足す
                auto up = db.prepare("UPDATE roots SET volume_id = ?, volume_name = ?, volume_rel_path = ? WHERE id = ?");
                up.bind(1, vol.id).bind(2, vol.name).bind(3, vol.rel).bind(4, id).run();
            }
            return id;
        }
        auto ins = db.prepare(
            "INSERT INTO roots (path, label, volume_id, volume_name, volume_rel_path) VALUES (?, ?, ?, ?, ?)");
        ins.bind(1, p).bind(2, l.empty() ? std::optional<std::string>() : std::optional<std::string>(l));
        ins.bind(3, vol.id).bind(4, vol.name).bind(5, vol.rel);
        ins.run();
        const int64_t id = db.last_insert_rowid();
        auto folder = db.prepare("INSERT INTO folders (root_id, parent_id, rel_path) VALUES (?, NULL, '')");
        folder.bind(1, id).run();
        // この中にある登録済みのルートを、新しいルートへ統合する
        for (const auto& [child_id, child_path] : all_root_paths(db))
            if (child_id != id && path_inside(p, child_path)) merge_root_into(db, id, p, child_id, child_path);
        return id;
    });
}

std::vector<Catalog::NestedRoot> Catalog::nested_roots() {
    std::lock_guard lock(reader_mutex_);
    const auto all = all_root_paths(*reader_);
    std::vector<NestedRoot> out;
    for (const auto& [cid, cpath] : all)
        for (const auto& [pid, ppath] : all)
            if (pid != cid && path_inside(ppath, cpath)) out.push_back({pid, cid});
    return out;
}

int Catalog::merge_nested_roots() {
    if (nested_roots().empty()) return 0;
    backup("merge-roots");
    return writer_->call([&](Database& db) -> int {
        int merged = 0;
        for (;;) {
            const auto all = all_root_paths(db);
            bool did = false;
            // いちばん外側のルートへ、内側のものを統合する（入れ子が 3 段でも順に畳む）
            for (const auto& [cid, cpath] : all) {
                const std::pair<int64_t, std::string>* best = nullptr;
                for (const auto& r : all)
                    if (r.first != cid && path_inside(r.second, cpath) && (!best || r.second.size() < best->second.size()))
                        best = &r;
                if (!best) continue;
                merge_root_into(db, best->first, best->second, cid, cpath);
                ++merged;
                did = true;
                break;
            }
            if (!did) break;
        }
        return merged;
    });
}

std::optional<Catalog::FolderLocation> Catalog::folder_for_path(const fs::path& dir) {
    const std::string p = normalized_path_string(dir);
    std::lock_guard lock(reader_mutex_);
    for (const auto& [id, path] : all_root_paths(*reader_)) {
        if (casefold_key(path) != casefold_key(p) && !path_inside(path, p)) continue;
        const size_t skip = path == "/" ? 1 : path.size() + 1;
        const std::string rel = p.size() > skip ? p.substr(skip) : std::string();
        auto st = reader_->prepare("SELECT id FROM folders WHERE root_id = ? AND rel_path = ?");
        st.bind(1, id).bind(2, to_nfc(rel));
        if (st.step()) return FolderLocation{id, st.column_int64(0)};
        return std::nullopt;
    }
    return std::nullopt;
}

void Catalog::relocate_root(int64_t root_id, const fs::path& new_dir) {
    std::error_code ec;
    if (!fs::is_directory(new_dir, ec)) throw Error(Error::Code::NotFound, "not a directory: " + path_to_utf8(new_dir));
    const std::string p = normalized_path_string(new_dir);
    const VolumeColumns vol = volume_columns(new_dir);
    writer_->call([&](Database& db) {
        bool known = false;
        for (const auto& [id, path] : all_root_paths(db)) {
            if (id == root_id) {
                known = true;
                continue;
            }
            if (casefold_key(path) == casefold_key(p) || path_inside(path, p) || path_inside(p, path))
                throw Error(Error::Code::InvalidArgument, "the new location overlaps another folder in the catalog: " + path);
        }
        if (!known) throw Error(Error::Code::NotFound, "unknown root id " + std::to_string(root_id));
        auto up = db.prepare("UPDATE roots SET path = ?, volume_id = ?, volume_name = ?, volume_rel_path = ? WHERE id = ?");
        up.bind(1, p).bind(2, vol.id).bind(3, vol.name).bind(4, vol.rel).bind(5, root_id).run();
    });
}

fs::path Catalog::backup(std::string_view reason) {
    std::string tag;
    for (char ch : reason) tag += (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-') ? ch : '-';
    const fs::path dir = path_.parent_path();
    const std::string base = path_to_utf8(path_.filename()) + ".before-" + tag + "-";
    // 日時だけでは同じ秒に 2 回呼ばれたときに重なる。同じ秒のものの連番は、既存の最大値の次にする
    // （古いものを消したあとに、空いた小さい番号へ戻ると、新しいものが名前の順で最古になって消えてしまう）
    const std::string stamp = local_timestamp_for_filename();
    int next = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = path_to_utf8(it->path().filename());
        const std::string head = base + stamp + "-";
        if (n.rfind(head, 0) == 0) next = std::max(next, std::atoi(n.c_str() + head.size()) + 1);
    }
    char seq[8];
    std::snprintf(seq, sizeof seq, "-%02d", next);
    const fs::path out = dir / utf8_to_path(base + stamp + seq + ".bak");
    {
        Database src(path_, Database::Mode::ReadOnly);  // 書き込みの接続とは別に、読み取りで一貫したコピーを作る
        src.exec("VACUUM INTO '" + [&] {
            std::string q = path_to_utf8(out), r;
            for (char ch : q) { if (ch == '\'') r += '\''; r += ch; }
            return r;
        }() + "';");
    }
    // 同じ理由の古いバックアップを、新しい 5 つだけ残す
    std::vector<fs::path> mine;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = path_to_utf8(it->path().filename());
        if (n.rfind(base, 0) == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, ".bak") == 0) mine.push_back(it->path());
    }
    std::sort(mine.begin(), mine.end());
    while (mine.size() > 5) {
        fs::remove(mine.front(), ec);
        mine.erase(mine.begin());
    }
    return out;
}

std::vector<RootInfo> Catalog::roots() {
    std::vector<RootInfo> out;
    {
        std::lock_guard lock(reader_mutex_);
        auto st = reader_->prepare(
            "SELECT id, path, COALESCE(label, ''), COALESCE(volume_id, ''), COALESCE(volume_name, ''),"
            " COALESCE(volume_rel_path, '') FROM roots ORDER BY path");
        while (st.step())
            out.push_back({st.column_int64(0), st.column_text(1), st.column_text(2), st.column_text(3),
                           st.column_text(4), st.column_text(5), true});
    }
    // 応答しないネットワークボリュームで待ち続けないよう、時間切れのものは未接続とみなす
    std::vector<std::string> paths;
    for (const auto& r : out) paths.push_back(r.path);
    const auto reachable = directories_reachable(paths, std::chrono::milliseconds(1500));
    for (size_t i = 0; i < out.size(); ++i) out[i].online = reachable[i];
    return out;
}

std::optional<RootInfo> Catalog::root_for_path(const fs::path& dir) {
    const std::string p = normalized_path_string(dir);
    for (auto& r : roots())
        if (r.path == p) return r;
    return std::nullopt;
}

std::optional<RootInfo> Catalog::root_containing(const fs::path& dir) {
    const std::string p = normalized_path_string(dir);
    std::optional<RootInfo> best;
    for (auto& r : roots()) {
        const bool inside = p == r.path || r.path == "/" || p.rfind(r.path + "/", 0) == 0;
        if (inside && (!best || r.path.size() > best->path.size())) best = r;
    }
    return best;
}

int Catalog::refresh_volumes() {
    const auto all = roots();
    const auto vols = mounted_volumes();
    int changed = 0;
    for (const auto& r : all) {
        try {
            if (r.volume_id.empty()) {
                // 登録したときにボリュームを判別できなかった（または v2 までのカタログ）。いま判別できれば足す
                if (!r.online) continue;
                const VolumeColumns vol = volume_columns(utf8_to_path(r.path));
                if (!vol.id) continue;
                writer_->call([&](Database& db) {
                    auto up = db.prepare("UPDATE roots SET volume_id = ?, volume_name = ?, volume_rel_path = ? WHERE id = ?");
                    up.bind(1, vol.id).bind(2, vol.name).bind(3, vol.rel).bind(4, r.id).run();
                });
                continue;
            }
            const auto it = std::find_if(vols.begin(), vols.end(), [&](const VolumeInfo& v) { return v.id == r.volume_id; });
            if (it == vols.end()) continue;  // 外れている
            const std::string path = normalized_path_string(it->mount_point / utf8_to_path(r.volume_rel_path));
            const std::string name = to_nfc(it->name);
            // いま開けるパスがすでに同じ場所を指していれば書き換えない（シンボリックリンク経由の登録などを変えない）
            std::error_code ec;
            const bool same_place = r.online && fs::weakly_canonical(utf8_to_path(r.path), ec) ==
                                                    fs::weakly_canonical(utf8_to_path(path), ec);
            const std::string new_path = same_place ? r.path : path;
            if (new_path == r.path && name == r.volume_name) continue;
            writer_->call([&](Database& db) {
                auto up = db.prepare("UPDATE roots SET path = ?, volume_name = ? WHERE id = ?");
                up.bind(1, new_path).bind(2, name).bind(3, r.id).run();
            });
            if (new_path != r.path) ++changed;
        } catch (const Error&) {
            // 別のルートがすでにその場所を使っている（UNIQUE 違反）など。そのルートは今回は変えない
        }
    }
    return changed;
}

void Catalog::remove_root(int64_t root_id) {
    backup("remove-root");  // 外すと、そのルートの写真の現像・★・フラグ・タグが消える。戻せるよう、先にバックアップを作る
    writer_->call([&](Database& db) {
        auto find = db.prepare("SELECT 1 FROM roots WHERE id = ?");
        find.bind(1, root_id);
        if (!find.step()) throw Error(Error::Code::NotFound, "unknown root id " + std::to_string(root_id));
        auto st = db.prepare("DELETE FROM roots WHERE id = ?");  // folders → photos → edits・タグ・アルバム所属は ON DELETE CASCADE
        st.bind(1, root_id).run();
    });
}

void Catalog::remove_photos(std::span<const int64_t> ids) {
    std::vector<int64_t> v(ids.begin(), ids.end());
    writer_->call([&](Database& db) {
        auto st = db.prepare("DELETE FROM photos WHERE id = ?");  // edits・タグ・アルバムの所属は ON DELETE CASCADE
        for (int64_t id : v) st.bind(1, id).run();
    });
}

RootDetails Catalog::root_details(int64_t root_id) {
    RootDetails d;
    bool found = false;
    for (auto& r : roots())
        if (r.id == root_id) {
            d.root = r;
            found = true;
        }
    if (!found) throw Error(Error::Code::NotFound, "unknown root id " + std::to_string(root_id));

    {
        std::lock_guard lock(reader_mutex_);
        auto st = reader_->prepare(
            "SELECT COUNT(*), COALESCE(SUM(p.status = 1), 0), COALESCE(SUM(p.file_size), 0),"
            " substr(MIN(p.capture_time), 1, 10), substr(MAX(p.capture_time), 1, 10)"
            " FROM photos p JOIN folders f ON f.id = p.folder_id WHERE f.root_id = ?");
        st.bind(1, root_id);
        if (st.step()) {
            d.photos = st.column_int64(0);
            d.missing = st.column_int64(1);
            d.total_file_bytes = st.column_int64(2);
            d.capture_from = st.column_opt_text(3).value_or("");
            d.capture_to = st.column_opt_text(4).value_or("");
        }
        auto ep = reader_->prepare(
            "SELECT COUNT(*) FROM photos p JOIN folders f ON f.id = p.folder_id WHERE f.root_id = ? AND ("
            " p.rating > 0 OR p.flag <> 0 OR EXISTS (SELECT 1 FROM edits e WHERE e.photo_id = p.id)"
            " OR EXISTS (SELECT 1 FROM photo_tags t WHERE t.photo_id = p.id))");
        ep.bind(1, root_id);
        if (ep.step()) d.edited_photos = ep.column_int64(0);
        auto fc = reader_->prepare("SELECT COUNT(*) FROM folders WHERE root_id = ? AND parent_id IS NOT NULL");
        fc.bind(1, root_id);
        if (fc.step()) d.folders = fc.column_int64(0);
    }

    // ボリュームの情報は、接続しているときだけ（外れた共有に触れて待たされない）
    if (d.root.online) {
        const fs::path disk = utf8_to_path(d.root.path);
        if (const auto v = volume_for_path(disk)) {
            d.mount_point = path_to_utf8(v->mount_point);
            d.fs_type = v->fs_type;
            d.kind = static_cast<int>(volume_kind(*v));
        }
        if (const auto s = disk_space(disk, std::chrono::milliseconds(1500))) {
            d.total_bytes = s->total;
            d.free_bytes = s->free;
        }
    }
    return d;
}

void Catalog::set_root_label(int64_t root_id, std::string_view label) {
    const std::string l = to_nfc(label);
    writer_->call([&](Database& db) {
        auto st = db.prepare("UPDATE roots SET label = ? WHERE id = ?");
        st.bind(1, l.empty() ? std::optional<std::string>() : std::optional<std::string>(l)).bind(2, root_id).run();
    });
}

ScanStats Catalog::scan_root(int64_t root_id, const ScanOptions& opt) {
    std::optional<RootInfo> root;
    for (auto& r : roots())
        if (r.id == root_id) root = r;
    if (!root) throw Error(Error::Code::NotFound, "unknown root id " + std::to_string(root_id));
    const auto disk_root = resolve_nfc_path(root->path);
    std::error_code ec;
    if (!disk_root || !fs::is_directory(*disk_root, ec))
        throw Error(Error::Code::NotFound, "root folder is not available: " + root->path);

    // 最近の取り込み（v3.16）: この回で写真を足したら、開始時刻を記録する（imported_at と同じ書式、UTC）
    const std::string started_at = utc_now_iso();

    std::vector<std::string> dirs;
    std::vector<FoundFile> files;
    walk(*disk_root, dirs, files);
    attach_companions(files);  // RAW と同じ名前の JPEG などは、その RAW の付属ファイルにする（v3.22）

    ScanStats stats;

    // ---- フォルダ: 完全一致、なければ大文字小文字を無視して照合し、なければ追加する
    const auto folder_ids = writer_->call([&](Database& db) {
        std::map<std::string, int64_t> exact;
        std::multimap<std::string, std::pair<int64_t, std::string>> folded;
        {
            auto st = db.prepare("SELECT id, rel_path FROM folders WHERE root_id = ?");
            st.bind(1, root_id);
            while (st.step()) {
                exact[st.column_text(1)] = st.column_int64(0);
                folded.emplace(casefold_key(st.column_text(1)), std::make_pair(st.column_int64(0), st.column_text(1)));
            }
        }
        std::set<int64_t> used;
        for (const auto& d : dirs)
            if (auto it = exact.find(d); it != exact.end()) used.insert(it->second);

        std::map<std::string, int64_t> ids;
        for (const auto& d : dirs) {
            if (auto it = exact.find(d); it != exact.end()) {
                ids[d] = it->second;
                continue;
            }
            std::optional<int64_t> renamed;
            auto range = folded.equal_range(casefold_key(d));
            for (auto it = range.first; it != range.second; ++it) {
                if (!used.count(it->second.first)) {
                    renamed = it->second.first;
                    break;
                }
            }
            const std::optional<int64_t> parent =
                d.empty() ? std::nullopt : std::optional<int64_t>(ids.at(parent_rel(d)));
            if (renamed) {
                auto st = db.prepare("UPDATE folders SET rel_path = ?, parent_id = ? WHERE id = ?");
                st.bind(1, d).bind(2, parent).bind(3, *renamed).run();
                used.insert(*renamed);
                ids[d] = *renamed;
            } else {
                auto st = db.prepare("INSERT INTO folders (root_id, parent_id, rel_path) VALUES (?, ?, ?)");
                st.bind(1, root_id).bind(2, parent).bind(3, d).run();
                ids[d] = db.last_insert_rowid();
                ++stats.folders_added;
            }
        }
        return ids;
    });

    // ---- 既存の写真を読み込んで、見つかったファイルと照合する
    struct Existing {
        int64_t id, folder_id, size, mtime;
        std::string name;
        int status;
        PhotoKind kind = PhotoKind::Raw;
        std::string companions;
        bool matched = false;
    };
    auto existing = writer_->call([&](Database& db) {
        std::vector<Existing> rows;
        auto st = db.prepare(
            "SELECT p.id, p.folder_id, p.file_size, p.file_mtime, p.file_name, p.status, p.kind,"
            " COALESCE(p.companions, '') FROM photos p"
            " JOIN folders f ON f.id = p.folder_id WHERE f.root_id = ?");
        st.bind(1, root_id);
        while (st.step())
            rows.push_back({st.column_int64(0), st.column_int64(1), st.column_int64(2), st.column_int64(3),
                            st.column_text(4), st.column_int(5), static_cast<PhotoKind>(st.column_int(6)),
                            st.column_text(7)});
        return rows;
    });

    std::unordered_map<int64_t, std::vector<size_t>> by_folder;
    for (size_t i = 0; i < existing.size(); ++i) by_folder[existing[i].folder_id].push_back(i);

    std::vector<Work> work;
    std::vector<std::pair<int64_t, std::string>> renames;  // 内容は変わらず名前の大文字小文字だけ変わった
    struct KindUpdate {
        int64_t id;
        PhotoKind kind;
        std::string companions;
    };
    std::vector<KindUpdate> kind_updates;  // 内容は変わらず、付属ファイル（RAW の JPEG など）だけ変わった

    auto make_work = [&](const FoundFile& f, int64_t folder_id) {
        Work w;
        w.folder_id = folder_id;
        w.name = f.name;
        w.nfc_path = join_path(root->path, f.folder_rel, f.name);
        w.disk = f.disk;
        w.st = f.st;
        w.kind = f.kind;
        w.companions = join_companions(f.companions);
        return w;
    };

    auto match = [&](const FoundFile& f, Existing& e) {
        e.matched = true;
        const bool same_content = e.size == f.st.size && e.mtime == f.st.mtime;
        const bool name_changed = e.name != f.name;
        if (name_changed) ++stats.renamed;
        if (same_content && e.status != static_cast<int>(PhotoStatus::Missing)) {
            ++stats.unchanged;
            if (name_changed) renames.emplace_back(e.id, f.name);
            if (e.kind != f.kind || e.companions != join_companions(f.companions))
                kind_updates.push_back({e.id, f.kind, join_companions(f.companions)});
            if (opt.thumbnails && e.status == static_cast<int>(PhotoStatus::Ok)) {
                Work w = make_work(f, e.folder_id);
                if (!opt.thumbnails->contains(thumbnail_key(w.nfc_path, w.st.size, w.st.mtime))) {
                    w.thumb_only = true;
                    w.photo_id = e.id;
                    work.push_back(std::move(w));
                }
            }
            return;
        }
        Work w = make_work(f, e.folder_id);
        w.photo_id = e.id;
        w.was_missing = e.status == static_cast<int>(PhotoStatus::Missing);
        work.push_back(std::move(w));
    };

    // フォルダごとに、まず完全一致、次に大文字小文字を無視した一致で照合する
    std::map<int64_t, std::vector<const FoundFile*>> files_by_folder;
    for (const auto& f : files) files_by_folder[folder_ids.at(f.folder_rel)].push_back(&f);
    for (auto& [folder_id, list] : files_by_folder) {
        auto& rows = by_folder[folder_id];
        std::vector<const FoundFile*> pending;
        for (const FoundFile* f : list) {
            auto it = std::find_if(rows.begin(), rows.end(),
                                   [&](size_t i) { return !existing[i].matched && existing[i].name == f->name; });
            if (it != rows.end())
                match(*f, existing[*it]);
            else
                pending.push_back(f);
        }
        for (const FoundFile* f : pending) {
            const std::string key = casefold_key(f->name);
            auto it = std::find_if(rows.begin(), rows.end(), [&](size_t i) {
                return !existing[i].matched && casefold_key(existing[i].name) == key;
            });
            if (it != rows.end()) {
                match(*f, existing[*it]);
                continue;
            }
            // 同じ名前（拡張子を除く）で種類が違う行があれば、その写真が別のファイルに替わったとみなして引き継ぐ（v3.22）。
            //   JPEG だけだった写真に同じ名前の RAW が来た → その行が RAW の写真になる
            //   RAW がなくなって、同じ名前の JPEG だけが残った → その行が JPEG の写真になる
            const std::string stem_key = casefold_key(file_stem(f->name));
            auto stem_it = std::find_if(rows.begin(), rows.end(), [&](size_t i) {
                return !existing[i].matched && ((existing[i].kind == PhotoKind::Raw) != (f->kind == PhotoKind::Raw)) &&
                       casefold_key(file_stem(existing[i].name)) == stem_key;
            });
            if (stem_it != rows.end())
                match(*f, existing[*stem_it]);
            else
                work.push_back(make_work(*f, folder_id));
        }
    }

    // 付属ファイル（RAW の JPEG など）になった名前の行がまだ残っていれば、その RAW の行に合わせる。
    // 行が残るのは、JPEG を先に登録したあとで RAW が戻ってきたとき（RAW の行は別にある）
    struct Absorb {
        int64_t drop_id, folder_id;
        std::string primary_name;
    };
    std::vector<Absorb> absorbs;
    for (const auto& f : files) {
        if (f.companions.empty()) continue;
        const int64_t folder_id = folder_ids.at(f.folder_rel);
        for (auto& e : existing) {
            if (e.matched || e.folder_id != folder_id) continue;
            const std::string key = casefold_key(e.name);
            if (std::any_of(f.companions.begin(), f.companions.end(),
                            [&](const std::string& c) { return casefold_key(c) == key; })) {
                e.matched = true;  // ファイルなしにしない
                absorbs.push_back({e.id, folder_id, f.name});
            }
        }
    }

    std::vector<int64_t> now_missing;
    for (const auto& e : existing)
        if (!e.matched && e.status != static_cast<int>(PhotoStatus::Missing)) now_missing.push_back(e.id);
    stats.missing = static_cast<int>(now_missing.size());

    if (!now_missing.empty() || !renames.empty() || !kind_updates.empty()) {
        writer_->call([&](Database& db) {
            auto miss = db.prepare("UPDATE photos SET status = 1 WHERE id = ?");
            for (int64_t id : now_missing) miss.bind(1, id).run();
            auto ren = db.prepare("UPDATE photos SET file_name = ? WHERE id = ?");
            for (const auto& [id, name] : renames) ren.bind(1, name).bind(2, id).run();
            auto upd = db.prepare("UPDATE photos SET kind = ?, companions = ? WHERE id = ?");
            for (const auto& u : kind_updates)
                upd.bind(1, static_cast<int64_t>(u.kind))
                    .bind(2, u.companions.empty() ? std::nullopt : std::optional<std::string>(u.companions))
                    .bind(3, u.id)
                    .run();
        });
    }

    process_works(*writer_, work, opt, stats);
    if (!absorbs.empty()) {
        writer_->call([&](Database& db) {
            auto find = db.prepare("SELECT id FROM photos WHERE folder_id = ? AND file_name = ? COLLATE NOCASE LIMIT 1");
            for (const auto& a : absorbs) {
                find.bind(1, a.folder_id).bind(2, a.primary_name);
                if (find.step()) absorb_photo_row(db, find.column_int64(0), a.drop_id);
                find.reset();
            }
        });
    }
    // ディスクにもうないフォルダのうち、写真（ファイルなしを含む）を 1 枚も持たないものはカタログから消す。
    // 消えたフォルダ・登録しなくなったライブラリ（.photoslibrary など）の跡がサイドバーに残らないようにする（v3.19）。
    // 写真を持つフォルダと、その親は残す（★・タグ・編集を持つ写真を巻き込まない）
    writer_->call([&](Database& db) {
        std::map<int64_t, std::optional<int64_t>> parent_of;
        std::set<int64_t> keep;
        auto fs_ = db.prepare("SELECT id, parent_id FROM folders WHERE root_id = ?");
        fs_.bind(1, root_id);
        while (fs_.step()) parent_of[fs_.column_int64(0)] = fs_.column_opt_int64(1);
        auto with_photos = db.prepare(
            "SELECT DISTINCT p.folder_id FROM photos p JOIN folders f ON f.id = p.folder_id WHERE f.root_id = ?");
        with_photos.bind(1, root_id);
        auto keep_with_ancestors = [&](int64_t id) {
            for (std::optional<int64_t> cur = id; cur && keep.insert(*cur).second;) {
                auto it = parent_of.find(*cur);
                cur = it == parent_of.end() ? std::nullopt : it->second;
            }
        };
        for (const auto& [d, id] : folder_ids) keep_with_ancestors(id);
        while (with_photos.step()) keep_with_ancestors(with_photos.column_int64(0));
        auto del = db.prepare("DELETE FROM folders WHERE id = ?");
        for (const auto& [id, parent] : parent_of)
            if (!keep.count(id)) del.bind(1, id).run();
    });
    if (stats.added > 0) {
        writer_->call([&](Database& db) {
            auto st = db.prepare("INSERT INTO meta (key, value) VALUES ('last_import_at', ?)"
                                 " ON CONFLICT(key) DO UPDATE SET value = excluded.value");
            st.bind(1, started_at).run();
        });
    }
    return stats;
}

// ---- アルバム（v3.16、v3.19 でフォルダ・スマートアルバムを追加）

namespace {

std::optional<int> album_kind(Database& db, int64_t id) {
    auto st = db.prepare("SELECT kind FROM albums WHERE id = ?");
    st.bind(1, id);
    if (st.step()) return st.column_int(0);
    return std::nullopt;
}

// 親に指定できるのはフォルダだけ
void check_parent(Database& db, std::optional<int64_t> parent) {
    if (!parent) return;
    const auto kind = album_kind(db, *parent);
    if (!kind) throw Error(Error::Code::NotFound, "unknown album id " + std::to_string(*parent));
    if (*kind != static_cast<int>(AlbumKind::Folder))
        throw Error(Error::Code::InvalidArgument, "only an album folder can contain albums");
}

void check_name_free(Database& db, const std::string& name, std::optional<int64_t> parent, int64_t except_id) {
    auto dup = db.prepare("SELECT 1 FROM albums WHERE name = ? AND parent_id IS ? AND id <> ?");
    dup.bind(1, name).bind(2, parent).bind(3, except_id);
    if (dup.step()) throw Error(Error::Code::InvalidArgument, "album already exists: " + name);
}

int64_t insert_album(Database& db, const std::string& name, std::optional<int64_t> parent, AlbumKind kind,
                     std::optional<std::string> query) {
    check_parent(db, parent);
    check_name_free(db, name, parent, 0);
    auto st = db.prepare(
        "INSERT INTO albums (name, parent_id, kind, query, sort_order)"
        " VALUES (?, ?, ?, ?, (SELECT COALESCE(MAX(sort_order), 0) + 1 FROM albums))");
    st.bind(1, name).bind(2, parent).bind(3, static_cast<int>(kind)).bind(4, query).run();
    return db.last_insert_rowid();
}

} // namespace

std::vector<AlbumInfo> Catalog::albums() {
    std::lock_guard lock(reader_mutex_);
    auto st = reader_->prepare(
        "SELECT a.id, a.name, a.parent_id, a.kind,"
        " COALESCE(a.cover_photo_id, (SELECT ap.photo_id FROM album_photos ap JOIN photos p ON p.id = ap.photo_id"
        "   WHERE ap.album_id = a.id ORDER BY p.capture_time IS NULL, p.capture_time, p.file_name, p.id LIMIT 1)),"
        " a.query,"
        " (SELECT COUNT(*) FROM album_photos ap WHERE ap.album_id = a.id)"
        " FROM albums a ORDER BY a.sort_order, a.id");
    std::vector<AlbumInfo> all;
    std::vector<std::optional<std::string>> queries;
    while (st.step()) {
        AlbumInfo a;
        a.id = st.column_int64(0);
        a.name = st.column_text(1);
        a.parent_id = st.column_opt_int64(2);
        a.kind = static_cast<AlbumKind>(st.column_int(3));
        a.cover_photo_id = st.column_opt_int64(4);
        a.photo_count = st.column_int64(6);
        queries.push_back(st.column_opt_text(5));
        all.push_back(std::move(a));
    }
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i].kind != AlbumKind::Smart) continue;
        const SqlClause c = smart_query_clause(queries[i].value_or(empty_smart_query()));
        WhereClause w;
        w.append(c);
        auto cnt = reader_->prepare("SELECT COUNT(*) FROM photos p WHERE 1 = 1" + w.sql);
        w.bind_all(cnt);
        cnt.step();
        all[i].photo_count = cnt.column_int64(0);
    }
    // 親が先、同じ親の中は作った順（深さ優先）
    std::vector<AlbumInfo> out;
    std::set<int64_t> placed;
    auto emit = [&](auto&& self, std::optional<int64_t> parent) -> void {
        for (const auto& a : all) {
            if (a.parent_id != parent || !placed.insert(a.id).second) continue;
            out.push_back(a);
            self(self, a.id);
        }
    };
    emit(emit, std::nullopt);
    return out;
}

int64_t Catalog::create_album(std::string_view name, std::optional<int64_t> parent_id) {
    const std::string n = album_name(name);
    return writer_->call([&](Database& db) { return insert_album(db, n, parent_id, AlbumKind::Album, std::nullopt); });
}

int64_t Catalog::create_album_folder(std::string_view name, std::optional<int64_t> parent_id) {
    const std::string n = album_name(name);
    return writer_->call([&](Database& db) { return insert_album(db, n, parent_id, AlbumKind::Folder, std::nullopt); });
}

int64_t Catalog::create_smart_album(std::string_view name, const std::string& query_json,
                                    std::optional<int64_t> parent_id) {
    const std::string n = album_name(name);
    validate_smart_query(query_json);
    return writer_->call(
        [&](Database& db) { return insert_album(db, n, parent_id, AlbumKind::Smart, query_json); });
}

void Catalog::set_smart_query(int64_t album_id, const std::string& query_json) {
    validate_smart_query(query_json);
    writer_->call([&](Database& db) {
        if (album_kind(db, album_id) != static_cast<int>(AlbumKind::Smart))
            throw Error(Error::Code::InvalidArgument, "not a smart album");
        auto st = db.prepare("UPDATE albums SET query = ? WHERE id = ?");
        st.bind(1, query_json).bind(2, album_id).run();
    });
}

std::optional<std::string> Catalog::smart_query(int64_t album_id) {
    std::lock_guard lock(reader_mutex_);
    return load_smart_query(*reader_, album_id);
}

void Catalog::rename_album(int64_t album_id, std::string_view name) {
    const std::string n = album_name(name);
    writer_->call([&](Database& db) {
        auto cur = db.prepare("SELECT parent_id FROM albums WHERE id = ?");
        cur.bind(1, album_id);
        if (!cur.step()) throw Error(Error::Code::NotFound, "unknown album id " + std::to_string(album_id));
        check_name_free(db, n, cur.column_opt_int64(0), album_id);
        auto st = db.prepare("UPDATE albums SET name = ? WHERE id = ?");
        st.bind(1, n).bind(2, album_id).run();
    });
}

void Catalog::delete_album(int64_t album_id) {
    writer_->call([&](Database& db) {
        auto st = db.prepare("DELETE FROM albums WHERE id = ?");  // 子のアルバムと album_photos は ON DELETE CASCADE
        st.bind(1, album_id).run();
    });
}

void Catalog::move_album(int64_t album_id, std::optional<int64_t> parent_id) {
    writer_->call([&](Database& db) {
        auto cur = db.prepare("SELECT name FROM albums WHERE id = ?");
        cur.bind(1, album_id);
        if (!cur.step()) throw Error(Error::Code::NotFound, "unknown album id " + std::to_string(album_id));
        const std::string name = cur.column_text(0);
        check_parent(db, parent_id);
        // 自分か自分の子孫の中へは動かせない
        for (std::optional<int64_t> p = parent_id; p;) {
            if (*p == album_id) throw Error(Error::Code::InvalidArgument, "cannot move an album into itself");
            auto up = db.prepare("SELECT parent_id FROM albums WHERE id = ?");
            up.bind(1, *p);
            p = up.step() ? up.column_opt_int64(0) : std::nullopt;
        }
        check_name_free(db, name, parent_id, album_id);
        auto st = db.prepare("UPDATE albums SET parent_id = ? WHERE id = ?");
        st.bind(1, parent_id).bind(2, album_id).run();
    });
}

void Catalog::add_to_album(int64_t album_id, std::span<const int64_t> ids) {
    std::vector<int64_t> v(ids.begin(), ids.end());
    writer_->call([&](Database& db) {
        const auto kind = album_kind(db, album_id);
        if (!kind) throw Error(Error::Code::NotFound, "unknown album id " + std::to_string(album_id));
        if (*kind != static_cast<int>(AlbumKind::Album))
            throw Error(Error::Code::InvalidArgument, "photos can only be added to an album");
        auto st = db.prepare("INSERT OR IGNORE INTO album_photos (album_id, photo_id) VALUES (?, ?)");
        for (int64_t id : v) st.bind(1, album_id).bind(2, id).run();
    });
}

void Catalog::remove_from_album(int64_t album_id, std::span<const int64_t> ids) {
    std::vector<int64_t> v(ids.begin(), ids.end());
    writer_->call([&](Database& db) {
        auto st = db.prepare("DELETE FROM album_photos WHERE album_id = ? AND photo_id = ?");
        for (int64_t id : v) st.bind(1, album_id).bind(2, id).run();
    });
}

void Catalog::move_album_order(int64_t album_id, int delta) {
    if (delta == 0) return;
    writer_->call([&](Database& db) {
        auto cur = db.prepare("SELECT parent_id FROM albums WHERE id = ?");
        cur.bind(1, album_id);
        if (!cur.step()) throw Error(Error::Code::NotFound, "unknown album id " + std::to_string(album_id));
        const std::optional<int64_t> parent = cur.column_opt_int64(0);
        std::vector<int64_t> ids;
        auto sib = db.prepare("SELECT id FROM albums WHERE parent_id IS ? ORDER BY sort_order, id");
        sib.bind(1, parent);
        while (sib.step()) ids.push_back(sib.column_int64(0));
        const auto at = std::find(ids.begin(), ids.end(), album_id) - ids.begin();
        const auto to = at + (delta < 0 ? -1 : 1);
        if (to < 0 || to >= static_cast<std::ptrdiff_t>(ids.size())) return;
        std::swap(ids[at], ids[to]);
        // 並びの値は同じ親の中でだけ比べる。重なりがないよう、連番にし直す
        auto up = db.prepare("UPDATE albums SET sort_order = ? WHERE id = ?");
        for (size_t i = 0; i < ids.size(); ++i) up.bind(1, static_cast<int64_t>(i + 1)).bind(2, ids[i]).run();
    });
}

void Catalog::set_album_cover(int64_t album_id, std::optional<int64_t> photo_id) {
    writer_->call([&](Database& db) {
        if (photo_id) {
            auto member = db.prepare("SELECT 1 FROM album_photos WHERE album_id = ? AND photo_id = ?");
            member.bind(1, album_id).bind(2, *photo_id);
            if (!member.step()) throw Error(Error::Code::InvalidArgument, "the photo is not in the album");
        }
        auto st = db.prepare("UPDATE albums SET cover_photo_id = ? WHERE id = ?");
        st.bind(1, photo_id).bind(2, album_id).run();
    });
}

std::optional<int64_t> Catalog::find_photo_by_identity(std::string_view file_name, int64_t file_size,
                                                       std::string_view capture_time) {
    std::lock_guard lock(reader_mutex_);
    auto st = reader_->prepare(
        "SELECT id FROM photos WHERE file_name = ? COLLATE NOCASE AND file_size = ? AND capture_time = ? LIMIT 1");
    st.bind(1, to_nfc(file_name)).bind(2, file_size).bind(3, capture_time);
    if (st.step()) return st.column_int64(0);
    return std::nullopt;
}

std::optional<int64_t> Catalog::find_photo(int64_t root_id, std::string_view folder_rel_path,
                                           std::string_view file_name) {
    std::lock_guard lock(reader_mutex_);
    auto st = reader_->prepare(
        "SELECT p.id FROM photos p JOIN folders f ON f.id = p.folder_id"
        " WHERE f.root_id = ? AND f.rel_path = ? AND p.file_name = ?");
    st.bind(1, root_id).bind(2, to_nfc(folder_rel_path)).bind(3, to_nfc(file_name));
    if (st.step()) return st.column_int64(0);
    return std::nullopt;
}

ScanStats Catalog::register_files(int64_t root_id, std::span<const fs::path> files, const ScanOptions& opt) {
    std::optional<RootInfo> root;
    for (auto& r : roots())
        if (r.id == root_id) root = r;
    if (!root) throw Error(Error::Code::NotFound, "unknown root id " + std::to_string(root_id));
    const auto disk_root = resolve_nfc_path(root->path);
    std::error_code ec;
    if (!disk_root || !fs::is_directory(*disk_root, ec))
        throw Error(Error::Code::NotFound, "root folder is not available: " + root->path);
    const std::string started_at = utc_now_iso();

    // 渡されたファイルを、ルートからの相対のフォルダ・名前（NFC）にする
    std::vector<FoundFile> found;
    for (const auto& f : files) {
        const auto rel_u8 = f.lexically_relative(*disk_root).generic_u8string();
        const std::string rel = to_nfc(std::string(reinterpret_cast<const char*>(rel_u8.data()), rel_u8.size()));
        const std::string name = to_nfc(path_to_utf8(f.filename()));
        if (rel.empty() || rel.rfind("..", 0) == 0)
            throw Error(Error::Code::InvalidArgument, "file is outside the root: " + path_to_utf8(f));
        const auto kind = photo_kind_for_name(name);
        if (!kind) throw Error(Error::Code::InvalidArgument, "not a photo file: " + path_to_utf8(f));
        const auto st = stat_file(f);
        if (!st) throw Error(Error::Code::NotFound, "no such file: " + path_to_utf8(f));
        found.push_back({parent_rel(rel), name, f, *st, *kind, {}});
    }
    attach_companions(found);  // 同じ名前の RAW があれば、JPEG などはその付属ファイルにする（v3.22）

    ScanStats stats;
    std::vector<Work> work;
    const auto folder_ids = writer_->call([&](Database& db) {
        std::map<std::string, int64_t> ids;  // 相対パス → フォルダ id（親が先）
        {
            auto st = db.prepare("SELECT id, rel_path FROM folders WHERE root_id = ?");
            st.bind(1, root_id);
            while (st.step()) ids[st.column_text(1)] = st.column_int64(0);
        }
        auto ensure = [&](auto&& self, const std::string& rel) -> int64_t {
            if (auto it = ids.find(rel); it != ids.end()) return it->second;
            const std::optional<int64_t> parent =
                rel.empty() ? std::nullopt : std::optional<int64_t>(self(self, parent_rel(rel)));
            auto ins = db.prepare("INSERT INTO folders (root_id, parent_id, rel_path) VALUES (?, ?, ?)");
            ins.bind(1, root_id).bind(2, parent).bind(3, rel).run();
            ++stats.folders_added;
            return ids[rel] = db.last_insert_rowid();
        };
        for (const auto& f : found) ensure(ensure, f.folder_rel);
        return ids;
    });

    // 既存の行（同じフォルダ・同じ名前）があれば、変わっていなければ飛ばし、変わっていれば読み直す
    writer_->call([&](Database& db) {
        auto q = db.prepare("SELECT id, file_size, file_mtime, status FROM photos WHERE folder_id = ? AND file_name = ?");
        for (const auto& f : found) {
            Work w;
            w.folder_id = folder_ids.at(f.folder_rel);
            w.name = f.name;
            w.nfc_path = join_path(root->path, f.folder_rel, f.name);
            w.disk = f.disk;
            w.st = f.st;
            w.kind = f.kind;
            w.companions = join_companions(f.companions);
            q.reset();
            q.bind(1, w.folder_id).bind(2, w.name);
            if (q.step()) {
                const bool same = q.column_int64(1) == f.st.size && q.column_int64(2) == f.st.mtime &&
                                  q.column_int(3) == static_cast<int>(PhotoStatus::Ok);
                if (same) {
                    ++stats.unchanged;
                    continue;
                }
                w.photo_id = q.column_int64(0);
                w.was_missing = q.column_int(3) == static_cast<int>(PhotoStatus::Missing);
            }
            work.push_back(std::move(w));
        }
    });

    process_works(*writer_, work, opt, stats);
    if (stats.added > 0) {
        writer_->call([&](Database& db) {
            auto st = db.prepare("INSERT INTO meta (key, value) VALUES ('last_import_at', ?)"
                                 " ON CONFLICT(key) DO UPDATE SET value = excluded.value");
            st.bind(1, started_at).run();
        });
    }
    return stats;
}

std::vector<FolderInfo> Catalog::folders(int64_t root_id) {
    std::lock_guard lock(reader_mutex_);
    auto st = reader_->prepare(
        "SELECT f.id, f.root_id, f.parent_id, f.rel_path, (SELECT COUNT(*) FROM photos p WHERE p.folder_id = f.id)"
        " FROM folders f WHERE f.root_id = ? ORDER BY f.rel_path");
    st.bind(1, root_id);
    std::vector<FolderInfo> out;
    while (st.step())
        out.push_back({st.column_int64(0), st.column_int64(1), st.column_opt_int64(2), st.column_text(3),
                       st.column_int64(4)});
    return out;
}

std::optional<int64_t> Catalog::folder_id(int64_t root_id, std::string_view rel_path) {
    std::lock_guard lock(reader_mutex_);
    auto st = reader_->prepare("SELECT id FROM folders WHERE root_id = ? AND rel_path = ?");
    st.bind(1, root_id).bind(2, to_nfc(rel_path));
    if (st.step()) return st.column_int64(0);
    return std::nullopt;
}

int64_t Catalog::count(const PhotoFilter& filter) {
    std::lock_guard lock(reader_mutex_);
    const WhereClause w = build_where(filter, *reader_);
    auto st = reader_->prepare("SELECT COUNT(*) FROM photos p" + w.sql);
    w.bind_all(st);
    st.step();
    return st.column_int64(0);
}

std::vector<PhotoRecord> Catalog::query(const PhotoFilter& filter, int64_t offset, int64_t limit) {
    std::lock_guard lock(reader_mutex_);
    const WhereClause w = build_where(filter, *reader_);
    auto st = reader_->prepare(std::string("SELECT ") + kPhotoColumns +
                               " FROM photos p JOIN folders f ON f.id = p.folder_id JOIN roots r ON r.id = f.root_id" +
                               w.sql +
                               " ORDER BY p.capture_time IS NULL, p.capture_time, p.file_name, p.id"
                               " LIMIT ? OFFSET ?");
    w.bind_all(st);
    const int n = static_cast<int>(w.args.size());
    st.bind(n + 1, limit).bind(n + 2, offset);
    std::vector<PhotoRecord> out;
    while (st.step()) out.push_back(read_photo(st));
    return out;
}

std::vector<int64_t> Catalog::query_ids(const PhotoFilter& filter) {
    std::lock_guard lock(reader_mutex_);
    const WhereClause w = build_where(filter, *reader_);
    auto st = reader_->prepare("SELECT p.id FROM photos p" + w.sql +
                               " ORDER BY p.capture_time IS NULL, p.capture_time, p.file_name, p.id");
    w.bind_all(st);
    std::vector<int64_t> out;
    while (st.step()) out.push_back(st.column_int64(0));
    return out;
}

std::vector<PhotoRecord> Catalog::photos_by_ids(std::span<const int64_t> ids) {
    std::unordered_map<int64_t, PhotoRecord> found;
    {
        std::lock_guard lock(reader_mutex_);
        // SQLite の変数の上限を超えないよう 500 件ずつ
        for (size_t start = 0; start < ids.size(); start += 500) {
            const size_t n = std::min<size_t>(500, ids.size() - start);
            std::string sql = std::string("SELECT ") + kPhotoColumns +
                              " FROM photos p JOIN folders f ON f.id = p.folder_id JOIN roots r ON r.id = f.root_id"
                              " WHERE p.id IN (";
            for (size_t i = 0; i < n; ++i) sql += i ? ",?" : "?";
            sql += ")";
            auto st = reader_->prepare(sql);
            for (size_t i = 0; i < n; ++i) st.bind(static_cast<int>(i + 1), ids[start + i]);
            while (st.step()) {
                PhotoRecord r = read_photo(st);
                found.emplace(r.id, std::move(r));
            }
        }
    }
    std::vector<PhotoRecord> out;
    out.reserve(ids.size());
    for (int64_t id : ids)
        if (auto it = found.find(id); it != found.end()) out.push_back(it->second);
    return out;
}

std::optional<PhotoRecord> Catalog::photo(int64_t id) {
    std::lock_guard lock(reader_mutex_);
    auto st = reader_->prepare(std::string("SELECT ") + kPhotoColumns +
                               " FROM photos p JOIN folders f ON f.id = p.folder_id JOIN roots r ON r.id = f.root_id"
                               " WHERE p.id = ?");
    st.bind(1, id);
    if (st.step()) return read_photo(st);
    return std::nullopt;
}

std::optional<fs::path> Catalog::photo_disk_path(int64_t id) {
    const auto p = photo(id);
    if (!p) return std::nullopt;
    return resolve_nfc_path(p->path);
}

void Catalog::set_rating(std::span<const int64_t> ids, int rating) {
    if (rating < 0 || rating > 5) throw Error(Error::Code::InvalidArgument, "rating must be 0..5");
    std::vector<int64_t> v(ids.begin(), ids.end());
    writer_->call([&](Database& db) {
        auto st = db.prepare("UPDATE photos SET rating = ? WHERE id = ?");
        for (int64_t id : v) st.bind(1, static_cast<int64_t>(rating)).bind(2, id).run();
    });
}

void Catalog::set_flag(std::span<const int64_t> ids, int flag) {
    if (flag < -1 || flag > 1) throw Error(Error::Code::InvalidArgument, "flag must be -1, 0 or 1");
    std::vector<int64_t> v(ids.begin(), ids.end());
    writer_->call([&](Database& db) {
        auto st = db.prepare("UPDATE photos SET flag = ? WHERE id = ?");
        for (int64_t id : v) st.bind(1, static_cast<int64_t>(flag)).bind(2, id).run();
    });
}

int64_t Catalog::ensure_tag(std::string_view path) {
    const auto parts = split_tag_path(path);
    return writer_->call([&](Database& db) {
        std::optional<int64_t> parent;
        for (const auto& name : parts) {
            auto id = find_tag_child(db, parent, name);
            if (!id) {
                auto st = db.prepare("INSERT INTO tags (parent_id, name) VALUES (?, ?)");
                st.bind(1, parent).bind(2, name).run();
                id = db.last_insert_rowid();
            }
            parent = id;
        }
        return *parent;
    });
}

std::optional<int64_t> Catalog::find_tag(std::string_view path) {
    const auto parts = split_tag_path(path);
    std::lock_guard lock(reader_mutex_);
    std::optional<int64_t> parent;
    for (const auto& name : parts) {
        parent = find_tag_child(*reader_, parent, name);
        if (!parent) return std::nullopt;
    }
    return parent;
}

std::vector<TagInfo> Catalog::tags() {
    std::lock_guard lock(reader_mutex_);
    auto st = reader_->prepare(std::string(kTagTreeSql) + " ORDER BY tree.path");
    std::vector<TagInfo> out;
    while (st.step()) out.push_back(read_tag(st));
    return out;
}

void Catalog::add_tag(std::span<const int64_t> ids, int64_t tag_id) {
    std::vector<int64_t> v(ids.begin(), ids.end());
    writer_->call([&](Database& db) {
        auto st = db.prepare("INSERT OR IGNORE INTO photo_tags (photo_id, tag_id) VALUES (?, ?)");
        for (int64_t id : v) st.bind(1, id).bind(2, tag_id).run();
    });
}

void Catalog::remove_tag(std::span<const int64_t> ids, int64_t tag_id) {
    std::vector<int64_t> v(ids.begin(), ids.end());
    writer_->call([&](Database& db) {
        auto st = db.prepare("DELETE FROM photo_tags WHERE photo_id = ? AND tag_id = ?");
        for (int64_t id : v) st.bind(1, id).bind(2, tag_id).run();
    });
}

std::vector<TagInfo> Catalog::photo_tags(int64_t photo_id) {
    std::lock_guard lock(reader_mutex_);
    auto st = reader_->prepare(std::string(kTagTreeSql) +
                               " WHERE tree.id IN (SELECT tag_id FROM photo_tags WHERE photo_id = ?) ORDER BY tree.path");
    st.bind(1, photo_id);
    std::vector<TagInfo> out;
    while (st.step()) out.push_back(read_tag(st));
    return out;
}

std::optional<std::string> Catalog::edit_json(int64_t photo_id) {
    std::lock_guard lock(reader_mutex_);
    auto st = reader_->prepare("SELECT settings FROM edits WHERE photo_id = ?");
    st.bind(1, photo_id);
    if (st.step()) return st.column_text(0);
    return std::nullopt;
}

void Catalog::save_edit(int64_t photo_id, int process_version, std::optional<std::string> json) {
    writer_->post([photo_id, process_version, json = std::move(json)](Database& db) {
        if (!json) {
            db.prepare("DELETE FROM edits WHERE photo_id = ?").bind(1, photo_id).run();
            return;
        }
        auto st = db.prepare(
            "INSERT INTO edits (photo_id, process_version, settings, updated_at)"
            " VALUES (?, ?, ?, strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))"
            " ON CONFLICT(photo_id) DO UPDATE SET process_version = excluded.process_version,"
            " settings = excluded.settings, updated_at = excluded.updated_at");
        st.bind(1, photo_id).bind(2, static_cast<int64_t>(process_version)).bind(3, *json).run();
    });
}

void Catalog::apply_preset(std::span<const int64_t> photo_ids, const Settings& preset) {
    for (const int64_t id : photo_ids) {
        // RAW 以外の写真（JPEG など）は現像の対象外（v3.22）
        if (const auto photo = this->photo(id); photo && photo->kind != PhotoKind::Raw) continue;
        const auto json = edit_json(id);
        const Settings merged = focal::apply_preset(json ? settings_from_json(*json) : Settings{}, preset);
        if (settings_need_no_row(merged))
            save_edit(id, merged.process_version, std::nullopt);
        else
            save_edit(id, merged.process_version, settings_to_json(merged));
    }
}

void Catalog::flush() {
    writer_->call([](Database&) {});
}

bool is_raw_file_name(std::string_view name) { return is_raw_file(std::string(name)); }

std::optional<std::string> capture_time_string(std::time_t t) { return local_time_string(t); }

} // namespace focal
