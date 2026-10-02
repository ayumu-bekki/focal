#include "catalog/sqlite.h"

#include <sqlite3.h>

#include "util/error.h"
#include "util/file.h"

namespace focal::db {

namespace {

[[noreturn]] void fail(sqlite3* db, std::string_view what) {
    throw Error(Error::Code::Database, std::string(what) + ": " + (db ? sqlite3_errmsg(db) : "sqlite error"));
}

} // namespace

Statement::Statement(sqlite3* db, std::string_view sql) : db_(db) {
    if (sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &stmt_, nullptr) != SQLITE_OK)
        fail(db, "prepare");
}

Statement::~Statement() { sqlite3_finalize(stmt_); }

Statement::Statement(Statement&& o) noexcept : db_(o.db_), stmt_(o.stmt_) { o.stmt_ = nullptr; }

Statement& Statement::operator=(Statement&& o) noexcept {
    if (this != &o) {
        sqlite3_finalize(stmt_);
        db_ = o.db_;
        stmt_ = o.stmt_;
        o.stmt_ = nullptr;
    }
    return *this;
}

Statement& Statement::bind(int index, int64_t v) {
    if (sqlite3_bind_int64(stmt_, index, v) != SQLITE_OK) fail(db_, "bind");
    return *this;
}

Statement& Statement::bind(int index, double v) {
    if (sqlite3_bind_double(stmt_, index, v) != SQLITE_OK) fail(db_, "bind");
    return *this;
}

Statement& Statement::bind(int index, std::string_view v) {
    if (sqlite3_bind_text(stmt_, index, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT) != SQLITE_OK)
        fail(db_, "bind");
    return *this;
}

Statement& Statement::bind_null(int index) {
    if (sqlite3_bind_null(stmt_, index) != SQLITE_OK) fail(db_, "bind");
    return *this;
}

bool Statement::step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    fail(db_, "step");
}

void Statement::run() {
    while (step()) {
    }
    reset();
}

void Statement::reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
}

int64_t Statement::column_int64(int col) const { return sqlite3_column_int64(stmt_, col); }
double Statement::column_double(int col) const { return sqlite3_column_double(stmt_, col); }

std::string Statement::column_text(int col) const {
    const auto* p = sqlite3_column_text(stmt_, col);
    return p ? std::string(reinterpret_cast<const char*>(p), sqlite3_column_bytes(stmt_, col)) : std::string();
}

bool Statement::column_is_null(int col) const { return sqlite3_column_type(stmt_, col) == SQLITE_NULL; }

std::optional<int64_t> Statement::column_opt_int64(int col) const {
    if (column_is_null(col)) return std::nullopt;
    return column_int64(col);
}

std::optional<double> Statement::column_opt_double(int col) const {
    if (column_is_null(col)) return std::nullopt;
    return column_double(col);
}

std::optional<std::string> Statement::column_opt_text(int col) const {
    if (column_is_null(col)) return std::nullopt;
    return column_text(col);
}

Database::Database(const std::filesystem::path& path, Mode mode) {
    const int flags = (mode == Mode::ReadOnly) ? SQLITE_OPEN_READONLY
                                                : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    const std::string p = path_to_utf8(path);
    if (sqlite3_open_v2(p.c_str(), &db_, flags | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "open failed";
        sqlite3_close(db_);
        db_ = nullptr;
        throw Error(Error::Code::Database, "cannot open catalog " + p + ": " + msg);
    }
    sqlite3_busy_timeout(db_, 5000);
    exec("PRAGMA foreign_keys = ON;");
}

Database::~Database() { sqlite3_close_v2(db_); }

void Database::exec(std::string_view sql) {
    char* err = nullptr;
    const std::string s(sql);
    if (sqlite3_exec(db_, s.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "exec failed";
        sqlite3_free(err);
        throw Error(Error::Code::Database, msg);
    }
}

int64_t Database::last_insert_rowid() const { return sqlite3_last_insert_rowid(db_); }
int Database::changes() const { return sqlite3_changes(db_); }

int Database::user_version() {
    auto st = prepare("PRAGMA user_version;");
    st.step();
    return st.column_int(0);
}

void Database::set_user_version(int v) { exec("PRAGMA user_version = " + std::to_string(v) + ";"); }

Transaction::Transaction(Database& db) : db_(db) { db_.exec("BEGIN IMMEDIATE;"); }

Transaction::~Transaction() {
    if (!done_) {
        try {
            db_.exec("ROLLBACK;");
        } catch (...) {
        }
    }
}

void Transaction::commit() {
    db_.exec("COMMIT;");
    done_ = true;
}

} // namespace focal::db
