#include "catalog/backup.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <thread>
#include <tuple>

#include "catalog/catalog.h"
#include "catalog/db_writer.h"
#include "catalog/sqlite.h"
#include "util/error.h"
#include "util/file.h"

namespace focal {

namespace fs = std::filesystem;

namespace {

constexpr const char* kExt = ".focalcatalog";

std::string quote_sql(const std::string& s) {
    std::string r = "'";
    for (char ch : s) {
        if (ch == '\'') r += '\'';
        r += ch;
    }
    return r + "'";
}

// "YYYY-MM-DD HHMM"（ローカル時刻）
std::string local_stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H%M", &tm);
    return buf;
}

std::string utc_now_iso_z() {
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

int64_t file_bytes(const fs::path& p) {
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    return ec ? 0 : static_cast<int64_t>(n);
}

struct ParsedName {
    std::string created;  // "YYYY-MM-DD HH:MM"
    int seq = 1;          // 同じ分の連番（なければ 1）
};

// "<name> YYYY-MM-DD HHMM[ N].focalcatalog" か。合えば日時と連番を返す
std::optional<ParsedName> parse_backup_name(const std::string& file_name, const std::string& catalog_name) {
    const std::string tail = kExt;
    if (file_name.size() <= tail.size() || file_name.compare(file_name.size() - tail.size(), tail.size(), tail) != 0)
        return std::nullopt;
    const std::string stem = file_name.substr(0, file_name.size() - tail.size());
    if (stem.rfind(catalog_name + " ", 0) != 0) return std::nullopt;
    const std::string rest = stem.substr(catalog_name.size() + 1);  // "YYYY-MM-DD HHMM" または "… N"
    int y, mo, d, h, mi, n = 0;
    char extra = 0;
    const int got = std::sscanf(rest.c_str(), "%4d-%2d-%2d %2d%2d %d%c", &y, &mo, &d, &h, &mi, &n, &extra);
    if (got < 5 || got > 6 || rest.size() < 15) return std::nullopt;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d", y, mo, d, h, mi);
    return ParsedName{buf, got == 6 ? n : 1};
}

} // namespace

std::vector<BackupInfo> list_backups(const fs::path& dir, const std::string& catalog_name) {
    std::vector<std::pair<int, BackupInfo>> found;  // 連番つき
    std::vector<BackupInfo> out;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return out;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const std::string name = path_to_utf8(it->path().filename());
        const auto created = parse_backup_name(name, catalog_name);
        if (!created) continue;
        const int64_t bytes = file_bytes(it->path() / "catalog.sqlite");
        if (bytes == 0) continue;  // 中身のないもの（途中で止まったもの）は一覧に出さない
        found.push_back({created->seq, {it->path(), created->created, bytes}});
    }
    // 日時の新しい順。同じ分の連番は、あとのものが新しい
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return std::tie(a.second.created, a.first) > std::tie(b.second.created, b.first);
    });
    for (auto& f : found) out.push_back(std::move(f.second));
    return out;
}

int prune_backups(const fs::path& dir, const std::string& catalog_name, int keep) {
    if (keep <= 0) return 0;
    const auto all = list_backups(dir, catalog_name);
    int removed = 0;
    for (size_t i = static_cast<size_t>(keep); i < all.size(); ++i) {
        std::error_code ec;
        fs::remove_all(all[i].path, ec);
        if (!ec) ++removed;
    }
    return removed;
}

BackupResult Catalog::backup_to(const fs::path& dest_dir, const BackupOptions& options) {
    BackupResult result;
    std::error_code ec;
    fs::create_directories(dest_dir, ec);
    if (ec || !fs::is_directory(dest_dir, ec))
        throw Error(Error::Code::Io, "cannot create the backup folder: " + path_to_utf8(dest_dir));
    auto cancelled = [&] { return options.cancel && options.cancel->load(); };
    if (cancelled()) throw Error(Error::Code::Cancelled, "backup cancelled");

    // 書き込みの接続とは別に、読み取りの接続で一貫したコピーを作る
    db::Database src(path_, db::Database::Mode::ReadOnly);

    if (options.check_integrity && !options.allow_damaged) {
        std::string message;
        bool ok = true;
        auto st = src.prepare("PRAGMA quick_check(5)");
        int lines = 0;
        while (st.step()) {
            const std::string line = st.column_text(0);
            if (lines++ == 0 && line == "ok") break;
            ok = false;
            message += (message.empty() ? "" : "\n") + line;
        }
        if (!ok) {
            result.skipped_damaged = true;
            result.integrity_message = message;
            return result;
        }
    }

    // 名前: <カタログ名> YYYY-MM-DD HHMM.focalcatalog（同じ分にもう一つあれば " 2"、" 3" …）
    const std::string base = name() + " " + local_stamp();
    fs::path final_dir = dest_dir / utf8_to_path(base + kExt);
    for (int n = 2; fs::exists(final_dir, ec); ++n) final_dir = dest_dir / utf8_to_path(base + " " + std::to_string(n) + kExt);
    const fs::path partial = fs::path(final_dir.native() + fs::path::string_type(".partial", ".partial" + 8));
    fs::remove_all(partial, ec);
    fs::create_directories(partial, ec);
    const fs::path out_file = partial / "catalog.sqlite";

    // 進み具合: 書いているファイルの大きさ ÷ 元の大きさ。キャンセルは sqlite3_interrupt で VACUUM を止める
    const int64_t source_bytes = std::max<int64_t>(1, file_bytes(path_) + file_bytes(fs::path(path_.native() + fs::path::string_type("-wal", "-wal" + 4))));
    std::atomic<bool> done{false};
    std::thread poller([&] {
        while (!done.load()) {
            if (cancelled()) sqlite3_interrupt(src.raw());
            if (options.progress)
                options.progress(std::min(0.99, static_cast<double>(file_bytes(out_file)) / static_cast<double>(source_bytes)));
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
        }
    });
    try {
        src.exec("VACUUM INTO " + quote_sql(path_to_utf8(out_file)) + ";");
    } catch (...) {
        done = true;
        poller.join();
        fs::remove_all(partial, ec);
        if (cancelled()) throw Error(Error::Code::Cancelled, "backup cancelled");
        throw;
    }
    done = true;
    poller.join();
    if (cancelled()) {
        fs::remove_all(partial, ec);
        throw Error(Error::Code::Cancelled, "backup cancelled");
    }
    fs::rename(partial, final_dir, ec);
    if (ec) {
        fs::remove_all(partial, ec);
        throw Error(Error::Code::Io, "cannot save the backup: " + path_to_utf8(final_dir));
    }
    if (options.progress) options.progress(1.0);

    writer_->call([&](db::Database& db) {
        auto st = db.prepare("INSERT INTO meta (key, value) VALUES ('last_backup_at', ?)"
                             " ON CONFLICT(key) DO UPDATE SET value = excluded.value");
        st.bind(1, utc_now_iso_z()).run();
    });
    result.path = final_dir;
    result.bytes = file_bytes(final_dir / "catalog.sqlite");
    return result;
}

} // namespace focal
