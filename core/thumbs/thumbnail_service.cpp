#include "thumbs/thumbnail_service.h"

#include <algorithm>

#include "catalog/catalog.h"
#include "util/file.h"

namespace focal {

ThumbnailService::ThumbnailService(Catalog& catalog, std::filesystem::path cache_dir, unsigned threads)
    : catalog_(catalog), cache_(std::move(cache_dir)) {
    if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency() - 1);
    for (unsigned i = 0; i < threads; ++i) workers_.emplace_back([this] { worker(); });
}

ThumbnailService::~ThumbnailService() {
    std::deque<Job> pending;
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        pending.swap(queue_);
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
    for (auto& job : pending) job.cb(job.id, Result::Cancelled, {});
}

uint64_t ThumbnailService::request(int64_t photo_id, Callback cb) {
    uint64_t id;
    bool stopped;
    {
        std::lock_guard lock(mutex_);
        id = next_id_++;
        stopped = stopping_;
        if (!stopped) queue_.push_back({id, photo_id, cb});
    }
    if (stopped) {
        cb(id, Result::Cancelled, {});  // ロックの外で呼ぶ（コールバックから再入してもよいように）
        return id;
    }
    cv_.notify_one();
    return id;
}

void ThumbnailService::cancel(uint64_t request_id) {
    Job job;
    {
        std::lock_guard lock(mutex_);
        auto it = std::find_if(queue_.begin(), queue_.end(), [&](const Job& j) { return j.id == request_id; });
        if (it == queue_.end()) return;  // 実行中か完了済み。実行中のものはそのまま完了させる
        job = std::move(*it);
        queue_.erase(it);
    }
    job.cb(job.id, Result::Cancelled, {});
}

void ThumbnailService::worker() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            job = std::move(queue_.back());
            queue_.pop_back();
        }
        run(job);
    }
}

void ThumbnailService::run(const Job& job) {
    try {
        const auto photo = catalog_.photo(job.photo_id);
        if (!photo || photo->status != PhotoStatus::Ok) {
            job.cb(job.id, Result::Failed, {});
            return;
        }
        // 1. 現像ビューアで表示したことがあれば、その現像結果（今の設定のもの）
        // 2. 編集済みなら、編集を反映して作る
        // 3. それ以外はカメラの埋め込みプレビュー（10 章）
        Settings settings;
        if (auto json = catalog_.edit_json(job.photo_id)) {
            try {
                settings = settings_from_json(*json);
            } catch (const std::exception&) {
            }
        }
        const bool edited = !settings_need_no_row(settings);
        const std::string rendered = rendered_key(photo->path, photo->file_size, photo->file_mtime, settings);
        const std::string key = (edited || cache_.contains(rendered))
                                    ? rendered
                                    : thumbnail_key(photo->path, photo->file_size, photo->file_mtime);
        const std::filesystem::path path = cache_.path_for(key);
        if (!cache_.contains(key)) {
            const auto disk = resolve_nfc_path(photo->path);
            if (!disk) {
                job.cb(job.id, Result::Failed, {});
                return;
            }
            ThumbnailOptions opt;
            if (edited) opt.settings = settings;
            cache_.store(key, make_thumbnail(*disk, opt).image);
        }
        job.cb(job.id, Result::Ok, path_to_utf8(path));
    } catch (const std::exception&) {
        job.cb(job.id, Result::Failed, {});
    }
}

} // namespace focal
