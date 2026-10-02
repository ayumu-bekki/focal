#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace focal::db {

// SQLite C API の薄い RAII ラッパー（ADR-07）。エラーは Error(Code::Database) を投げる。

class Statement {
public:
    Statement() = default;
    Statement(sqlite3* db, std::string_view sql);
    ~Statement();
    Statement(Statement&& o) noexcept;
    Statement& operator=(Statement&& o) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    // バインドの添字は 1 から（SQLite と同じ）
    Statement& bind(int index, int64_t v);
    Statement& bind(int index, int v) { return bind(index, static_cast<int64_t>(v)); }
    Statement& bind(int index, double v);
    Statement& bind(int index, std::string_view v);
    Statement& bind(int index, const char* v) { return bind(index, std::string_view(v)); }
    Statement& bind(int index, const std::string& v) { return bind(index, std::string_view(v)); }
    Statement& bind_null(int index);
    template <class T>
    Statement& bind(int index, const std::optional<T>& v) {
        return v ? bind(index, *v) : bind_null(index);
    }

    // 行があれば true、終わりなら false
    bool step();
    // 結果行を返さない文を実行する
    void run();
    void reset();

    int64_t column_int64(int col) const;
    int column_int(int col) const { return static_cast<int>(column_int64(col)); }
    double column_double(int col) const;
    std::string column_text(int col) const;
    bool column_is_null(int col) const;
    std::optional<int64_t> column_opt_int64(int col) const;
    std::optional<double> column_opt_double(int col) const;
    std::optional<std::string> column_opt_text(int col) const;

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

class Database {
public:
    enum class Mode { ReadWrite, ReadOnly };

    // ReadWrite はファイルがなければ作る。接続ごとに foreign_keys = ON と busy_timeout を設定する（7.1 章）。
    Database(const std::filesystem::path& path, Mode mode);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void exec(std::string_view sql);
    Statement prepare(std::string_view sql) { return Statement(db_, sql); }
    int64_t last_insert_rowid() const;
    int changes() const;
    int user_version();
    void set_user_version(int v);
    sqlite3* raw() const { return db_; }

private:
    sqlite3* db_ = nullptr;
};

// BEGIN IMMEDIATE / COMMIT。commit() されずに破棄されたら ROLLBACK する。
class Transaction {
public:
    explicit Transaction(Database& db);
    ~Transaction();
    void commit();

private:
    Database& db_;
    bool done_ = false;
};

} // namespace focal::db
