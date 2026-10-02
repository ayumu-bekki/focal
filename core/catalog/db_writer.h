#pragma once

#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>

#include "catalog/sqlite.h"

namespace focal::db {

// 書き込み専用の接続とスレッド（4.2 章、ADR-07）。
// 積まれたジョブを最大 kBatch 件ずつ 1 トランザクションにまとめて実行する。
// ジョブごとに SAVEPOINT を張るので、1 件の失敗は他のジョブに影響しない。
// 結果（future）はコミット後に返るので、呼び出し側は直後に読み取り接続から結果を読める。
class DbWriter {
public:
    static constexpr size_t kBatch = 500;

    // 接続を開いてスキーマを最新にする
    explicit DbWriter(const std::filesystem::path& path);
    ~DbWriter();
    DbWriter(const DbWriter&) = delete;
    DbWriter& operator=(const DbWriter&) = delete;

    std::future<void> post(std::function<void(Database&)> job);

    // 書き込みスレッドで fn を実行し、結果を待って返す
    template <class F>
    auto call(F&& fn) -> std::invoke_result_t<F, Database&> {
        using R = std::invoke_result_t<F, Database&>;
        if constexpr (std::is_void_v<R>) {
            post(std::forward<F>(fn)).get();
        } else {
            auto result = std::make_shared<std::optional<R>>();
            post([result, f = std::forward<F>(fn)](Database& db) mutable { result->emplace(f(db)); }).get();
            return std::move(**result);
        }
    }

    const std::filesystem::path& backup_path() const { return backup_; }

private:
    struct Job {
        std::function<void(Database&)> fn;
        std::promise<void> done;
    };

    void loop();

    std::unique_ptr<Database> db_;
    std::filesystem::path backup_;
    std::deque<Job> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false;
    std::thread thread_;
};

} // namespace focal::db
