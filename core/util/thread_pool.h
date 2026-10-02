#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace focal {

// latest-wins 用の打ち切り判定。世代番号が進んでいたら古い要求は打ち切る（4.2 章）。
class CancelToken {
public:
    CancelToken() = default;
    CancelToken(const std::atomic<uint64_t>* current, uint64_t mine) : current_(current), mine_(mine) {}

    bool cancelled() const {
        return current_ != nullptr && current_->load(std::memory_order_relaxed) != mine_;
    }

private:
    const std::atomic<uint64_t>* current_ = nullptr;
    uint64_t mine_ = 0;
};

class ThreadPool {
public:
    explicit ThreadPool(unsigned threads = 0);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    unsigned size() const { return static_cast<unsigned>(workers_.size()); }

    // [0, count) を grain 単位のブロックに分けて並列実行し、すべて終わるまで待つ。
    // 呼び出しスレッドも処理に参加する。
    void parallel_for(int count, int grain, const std::function<void(int begin, int end)>& fn);

    // プロセス全体で共有する画像処理用プール。
    static ThreadPool& shared();

private:
    void worker_loop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false;
};

} // namespace focal
