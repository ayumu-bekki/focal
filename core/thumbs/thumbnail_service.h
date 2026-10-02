#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "thumbs/thumbnail.h"

namespace focal {

class Catalog;

// グリッド表示用のサムネイル要求キュー（10 章）。
// - 後から来た要求を先に処理する（スクロール中は今見えている写真を優先する）
// - 開始前の要求はキャンセルできる（画面外に出たセル）
// - キャッシュにあればすぐ返し、なければ作ってキャッシュに入れてから返す
// コールバックはワーカースレッドから呼ばれる。どの要求にも必ず 1 回だけ呼ばれる。
class ThumbnailService {
public:
    enum class Result { Ok, Cancelled, Failed };
    // path はコールバックの間だけ有効（キャッシュの JPEG）
    using Callback = std::function<void(uint64_t request_id, Result result, const std::string& path_utf8)>;

    ThumbnailService(Catalog& catalog, std::filesystem::path cache_dir, unsigned threads = 0);
    // 開始前の要求はキャンセル扱いでコールバックし、実行中の要求の完了を待つ
    ~ThumbnailService();
    ThumbnailService(const ThumbnailService&) = delete;
    ThumbnailService& operator=(const ThumbnailService&) = delete;

    uint64_t request(int64_t photo_id, Callback cb);
    void cancel(uint64_t request_id);

    const ThumbnailCache& cache() const { return cache_; }

private:
    struct Job {
        uint64_t id;
        int64_t photo_id;
        Callback cb;
    };

    void worker();
    void run(const Job& job);

    Catalog& catalog_;
    ThumbnailCache cache_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> queue_;  // 後ろが新しい。後ろから取り出す
    uint64_t next_id_ = 1;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

} // namespace focal
