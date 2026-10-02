#include "util/thread_pool.h"

#include <algorithm>
#include <memory>

namespace focal {

ThreadPool::ThreadPool(unsigned threads) {
    if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
    // 呼び出しスレッドも働くので、ワーカーは 1 本少なくする
    for (unsigned i = 0; i + 1 < threads; ++i) workers_.emplace_back([this] { worker_loop(); });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
}

void ThreadPool::worker_loop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) return;
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();
    }
}

void ThreadPool::parallel_for(int count, int grain, const std::function<void(int, int)>& fn) {
    if (count <= 0) return;
    grain = std::max(1, grain);
    const int blocks = (count + grain - 1) / grain;
    if (blocks == 1 || workers_.empty()) {
        fn(0, count);
        return;
    }

    struct Shared {
        std::atomic<int> next{0};
        std::atomic<int> done{0};
        std::mutex m;
        std::condition_variable cv;
        std::exception_ptr error;
    };
    auto shared = std::make_shared<Shared>();

    auto run = [shared, blocks, grain, count, &fn] {
        for (;;) {
            const int b = shared->next.fetch_add(1);
            if (b >= blocks) return;
            try {
                fn(b * grain, std::min(count, (b + 1) * grain));
            } catch (...) {
                std::lock_guard lock(shared->m);
                if (!shared->error) shared->error = std::current_exception();
            }
            if (shared->done.fetch_add(1) + 1 == blocks) {
                std::lock_guard lock(shared->m);
                shared->cv.notify_all();
            }
        }
    };

    const int helpers = std::min<int>(static_cast<int>(workers_.size()), blocks - 1);
    {
        std::lock_guard lock(mutex_);
        for (int i = 0; i < helpers; ++i) tasks_.push(run);
    }
    cv_.notify_all();
    run();

    std::unique_lock lock(shared->m);
    shared->cv.wait(lock, [&] { return shared->done.load() == blocks; });
    if (shared->error) std::rethrow_exception(shared->error);
}

ThreadPool& ThreadPool::shared() {
    static ThreadPool pool;
    return pool;
}

} // namespace focal
