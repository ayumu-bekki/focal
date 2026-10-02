#include "catalog/catalog.h"

#include <algorithm>
#include <ctime>
#include <map>
#include <set>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <variant>

#include "catalog/db_writer.h"
#include "catalog/sqlite.h"
#include "imaging/libraw_util.h"
#include "thumbs/thumbnail.h"
#include "util/error.h"
#include "util/file.h"
#include "util/hash.h"
#include "util/thread_pool.h"
#include "util/unicode.h"

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

const std::set<std::string>& raw_extensions() {
    static const std::set<std::string> exts = {
        "3fr", "arw", "cr2", "cr3", "crw", "dcr", "dng", "erf", "fff", "gpr", "iiq", "kdc", "mef", "mos",
        "mrw", "nef", "nrw", "orf", "pef", "raf", "raw", "rw2", "rwl", "sr2", "srf", "srw", "x3f",
    };
    return exts;
}

bool is_raw_file(const std::string& name) {
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return raw_extensions().count(ext) > 0;
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
};

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
            if (it->is_symlink(ec)) {
                it.disable_recursion_pending();  // シンボリックリンクのフォルダはたどらない（循環を避ける）
                continue;
            }
            dirs.push_back(rel);
        } else if (is_raw_file(name)) {
            if (auto st = stat_file(it->path())) files.push_back({parent_rel(rel), name, it->path(), *st});
        }
    }
    std::sort(dirs.begin(), dirs.end());  // 親が子より先に来る
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

    if (w.photo_id) {
        auto st = db.prepare(
            "UPDATE photos SET file_name = ?, file_size = ?, file_mtime = ?, quick_hash = ?, status = ?,"
            " capture_time = ?, camera_make = ?, camera_model = ?, lens_model = ?, iso = ?, exposure_time = ?,"
            " f_number = ?, focal_length = ?, width = ?, height = ?, orientation = ? WHERE id = ?");
        st.bind(1, w.name).bind(2, w.st.size).bind(3, w.st.mtime).bind(4, hash).bind(5, status);
        const int next = bind_meta(st, 6);
        st.bind(next, *w.photo_id);
        st.run();
    } else {
        auto st = db.prepare(
            "INSERT INTO photos (folder_id, file_name, file_size, file_mtime, quick_hash, status, capture_time,"
            " camera_make, camera_model, lens_model, iso, exposure_time, f_number, focal_length, width, height,"
            " orientation) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        st.bind(1, w.folder_id).bind(2, w.name).bind(3, w.st.size).bind(4, w.st.mtime).bind(5, hash).bind(6, status);
        bind_meta(st, 7);
        st.run();
    }
}

// ---- クエリ ------------------------------------------------------------

constexpr const char* kPhotoColumns =
    "p.id, p.folder_id, p.file_name, p.file_size, p.file_mtime, p.quick_hash, p.status, p.capture_time,"
    " p.camera_make, p.camera_model, p.lens_model, p.iso, p.exposure_time, p.f_number, p.focal_length,"
    " p.width, p.height, p.orientation, p.rating, p.flag, r.path, f.rel_path";

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
    return r;
}

// WHERE 句とバインドする値を組み立てる
struct WhereClause {
    std::string sql;
    std::vector<std::variant<int64_t, std::string>> args;

    void bind_all(Statement& st, int first = 1) const {
        int i = first;
        for (const auto& a : args) {
            if (std::holds_alternative<int64_t>(a))
                st.bind(i++, std::get<int64_t>(a));
            else
                st.bind(i++, std::get<std::string>(a));
        }
    }
};

WhereClause build_where(const PhotoFilter& f) {
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

// ---- Catalog -----------------------------------------------------------

std::unique_ptr<Catalog> Catalog::open(const fs::path& path) { return std::unique_ptr<Catalog>(new Catalog(path)); }

Catalog::Catalog(const fs::path& path) : path_(path) {
    if (path.has_parent_path()) fs::create_directories(path.parent_path());
    writer_ = std::make_unique<db::DbWriter>(path);  // スキーマを作ってから読み取り接続を開く
    reader_ = std::make_unique<Database>(path, Database::Mode::ReadOnly);
}

Catalog::~Catalog() = default;

fs::path Catalog::migration_backup() const { return writer_->backup_path(); }

int64_t Catalog::add_root(const fs::path& dir, std::string_view label) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) throw Error(Error::Code::NotFound, "not a directory: " + path_to_utf8(dir));
    const std::string p = normalized_path_string(dir);
    const std::string l = to_nfc(label);
    return writer_->call([&](Database& db) -> int64_t {
        auto find = db.prepare("SELECT id FROM roots WHERE path = ?");
        find.bind(1, p);
        if (find.step()) return find.column_int64(0);
        auto ins = db.prepare("INSERT INTO roots (path, label) VALUES (?, ?)");
        ins.bind(1, p).bind(2, l.empty() ? std::optional<std::string>() : std::optional<std::string>(l));
        ins.run();
        const int64_t id = db.last_insert_rowid();
        auto folder = db.prepare("INSERT INTO folders (root_id, parent_id, rel_path) VALUES (?, NULL, '')");
        folder.bind(1, id).run();
        return id;
    });
}

std::vector<RootInfo> Catalog::roots() {
    std::lock_guard lock(reader_mutex_);
    auto st = reader_->prepare("SELECT id, path, COALESCE(label, '') FROM roots ORDER BY path");
    std::vector<RootInfo> out;
    while (st.step()) out.push_back({st.column_int64(0), st.column_text(1), st.column_text(2)});
    return out;
}

std::optional<RootInfo> Catalog::root_for_path(const fs::path& dir) {
    const std::string p = normalized_path_string(dir);
    for (auto& r : roots())
        if (r.path == p) return r;
    return std::nullopt;
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
        bool matched = false;
    };
    auto existing = writer_->call([&](Database& db) {
        std::vector<Existing> rows;
        auto st = db.prepare(
            "SELECT p.id, p.folder_id, p.file_size, p.file_mtime, p.file_name, p.status FROM photos p"
            " JOIN folders f ON f.id = p.folder_id WHERE f.root_id = ?");
        st.bind(1, root_id);
        while (st.step())
            rows.push_back({st.column_int64(0), st.column_int64(1), st.column_int64(2), st.column_int64(3),
                            st.column_text(4), st.column_int(5)});
        return rows;
    });

    std::unordered_map<int64_t, std::vector<size_t>> by_folder;
    for (size_t i = 0; i < existing.size(); ++i) by_folder[existing[i].folder_id].push_back(i);

    std::vector<Work> work;
    std::vector<std::pair<int64_t, std::string>> renames;  // 内容は変わらず名前の大文字小文字だけ変わった

    auto make_work = [&](const FoundFile& f, int64_t folder_id) {
        Work w;
        w.folder_id = folder_id;
        w.name = f.name;
        w.nfc_path = join_path(root->path, f.folder_rel, f.name);
        w.disk = f.disk;
        w.st = f.st;
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
            } else {
                work.push_back(make_work(*f, folder_id));
            }
        }
    }

    std::vector<int64_t> now_missing;
    for (const auto& e : existing)
        if (!e.matched && e.status != static_cast<int>(PhotoStatus::Missing)) now_missing.push_back(e.id);
    stats.missing = static_cast<int>(now_missing.size());

    if (!now_missing.empty() || !renames.empty()) {
        writer_->call([&](Database& db) {
            auto miss = db.prepare("UPDATE photos SET status = 1 WHERE id = ?");
            for (int64_t id : now_missing) miss.bind(1, id).run();
            auto ren = db.prepare("UPDATE photos SET file_name = ? WHERE id = ?");
            for (const auto& [id, name] : renames) ren.bind(1, name).bind(2, id).run();
        });
    }

    // ---- 新規・変更されたファイルを並列に読み、チャンクごとに書き込む
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
        writer_->call([&](Database& db) {
            for (int i = start; i < end; ++i)
                if (!work[i].thumb_only) write_probe(db, work[i], probes[i - start]);
        });
        if (opt.progress) opt.progress(end, total);
    }
    if (stats.added > 0) {
        writer_->call([&](Database& db) {
            auto st = db.prepare("INSERT INTO meta (key, value) VALUES ('last_import_at', ?)"
                                 " ON CONFLICT(key) DO UPDATE SET value = excluded.value");
            st.bind(1, started_at).run();
        });
    }
    return stats;
}

// ---- アルバム（v3.16）

std::vector<AlbumInfo> Catalog::albums() {
    std::lock_guard lock(reader_mutex_);
    auto st = reader_->prepare(
        "SELECT a.id, a.name, (SELECT COUNT(*) FROM album_photos ap WHERE ap.album_id = a.id)"
        " FROM albums a ORDER BY a.sort_order, a.id");
    std::vector<AlbumInfo> out;
    while (st.step()) out.push_back({st.column_int64(0), st.column_text(1), st.column_int64(2)});
    return out;
}

int64_t Catalog::create_album(std::string_view name) {
    const std::string n = album_name(name);
    return writer_->call([&](Database& db) -> int64_t {
        auto dup = db.prepare("SELECT 1 FROM albums WHERE name = ?");
        dup.bind(1, n);
        if (dup.step()) throw Error(Error::Code::InvalidArgument, "album already exists: " + n);
        auto st = db.prepare("INSERT INTO albums (name, sort_order) VALUES (?, (SELECT COALESCE(MAX(sort_order), 0) + 1 FROM albums))");
        st.bind(1, n).run();
        return db.last_insert_rowid();
    });
}

void Catalog::rename_album(int64_t album_id, std::string_view name) {
    const std::string n = album_name(name);
    writer_->call([&](Database& db) {
        auto dup = db.prepare("SELECT 1 FROM albums WHERE name = ? AND id <> ?");
        dup.bind(1, n).bind(2, album_id);
        if (dup.step()) throw Error(Error::Code::InvalidArgument, "album already exists: " + n);
        auto st = db.prepare("UPDATE albums SET name = ? WHERE id = ?");
        st.bind(1, n).bind(2, album_id).run();
    });
}

void Catalog::delete_album(int64_t album_id) {
    writer_->call([&](Database& db) {
        auto st = db.prepare("DELETE FROM albums WHERE id = ?");  // album_photos は ON DELETE CASCADE
        st.bind(1, album_id).run();
    });
}

void Catalog::add_to_album(int64_t album_id, std::span<const int64_t> ids) {
    std::vector<int64_t> v(ids.begin(), ids.end());
    writer_->call([&](Database& db) {
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
    const WhereClause w = build_where(filter);
    std::lock_guard lock(reader_mutex_);
    auto st = reader_->prepare("SELECT COUNT(*) FROM photos p" + w.sql);
    w.bind_all(st);
    st.step();
    return st.column_int64(0);
}

std::vector<PhotoRecord> Catalog::query(const PhotoFilter& filter, int64_t offset, int64_t limit) {
    const WhereClause w = build_where(filter);
    std::lock_guard lock(reader_mutex_);
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
    const WhereClause w = build_where(filter);
    std::lock_guard lock(reader_mutex_);
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

void Catalog::flush() {
    writer_->call([](Database&) {});
}

} // namespace focal
