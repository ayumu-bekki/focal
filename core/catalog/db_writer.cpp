#include "catalog/db_writer.h"

#include <vector>

#include "catalog/schema.h"

namespace focal::db {

DbWriter::DbWriter(const std::filesystem::path& path)
    : db_(std::make_unique<Database>(path, Database::Mode::ReadWrite)) {
    backup_ = migrate(*db_, path, catalog_migrations());
    db_->exec("PRAGMA synchronous = NORMAL;");
    thread_ = std::thread([this] { loop(); });
}

DbWriter::~DbWriter() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

std::future<void> DbWriter::post(std::function<void(Database&)> job) {
    Job j{std::move(job), {}};
    auto f = j.done.get_future();
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(j));
    }
    cv_.notify_one();
    return f;
}

void DbWriter::call_outside_transaction(std::function<void(Database&)> job) {
    Job j;
    j.fn = std::move(job);
    j.bare = true;
    auto f = j.done.get_future();
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(j));
    }
    cv_.notify_one();
    f.get();
}

void DbWriter::loop() {
    for (;;) {
        std::vector<Job> batch;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) return;  // stopping_ かつキューが空
            while (!queue_.empty() && batch.size() < kBatch) {
                // 単独で実行するジョブは、前のジョブをコミットしてから（バッチの先頭になってから）、1 件だけ実行する
                if (queue_.front().bare && !batch.empty()) break;
                const bool bare = queue_.front().bare;
                batch.push_back(std::move(queue_.front()));
                queue_.pop_front();
                if (bare) break;
            }
        }
        if (batch.size() == 1 && batch[0].bare) {
            try {
                batch[0].fn(*db_);
                batch[0].done.set_value();
            } catch (...) {
                batch[0].done.set_exception(std::current_exception());
            }
            continue;
        }

        std::vector<std::exception_ptr> errors(batch.size());
        try {
            Transaction tx(*db_);
            for (size_t i = 0; i < batch.size(); ++i) {
                db_->exec("SAVEPOINT job;");
                try {
                    batch[i].fn(*db_);
                    db_->exec("RELEASE job;");
                } catch (...) {
                    errors[i] = std::current_exception();
                    db_->exec("ROLLBACK TO job; RELEASE job;");
                }
            }
            tx.commit();
        } catch (...) {
            // トランザクション自体が失敗した場合は、全ジョブを失敗にする
            for (auto& e : errors)
                if (!e) e = std::current_exception();
        }

        for (size_t i = 0; i < batch.size(); ++i) {
            if (errors[i])
                batch[i].done.set_exception(errors[i]);
            else
                batch[i].done.set_value();
        }
    }
}

} // namespace focal::db
