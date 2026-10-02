#pragma once

namespace focal {

// このスレッドから始まる OpenMP の並列領域（LibRaw 内部）のスレッド数を、スコープの間だけ変える。
// サムネイルプールなど、既に並列に動いているスレッドで LibRaw を使うときにスレッド数が掛け算で増えないようにする
// （design.md 5.1 章）。OpenMP なしでビルドした場合は何もしない。
class ScopedOmpThreads {
public:
    explicit ScopedOmpThreads(int threads);
    ~ScopedOmpThreads();
    ScopedOmpThreads(const ScopedOmpThreads&) = delete;
    ScopedOmpThreads& operator=(const ScopedOmpThreads&) = delete;

private:
    int previous_ = 0;
};

} // namespace focal
