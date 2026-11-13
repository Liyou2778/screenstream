#pragma once

#include "common.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <thread>

namespace ss {

// Minimal fixed-size worker pool. Long lived connection handlers are submitted as tasks;
// the pool bounds how many connections can be streamed at the same time.
class ThreadPool {
public:
    explicit ThreadPool(size_t threads);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void Submit(std::function<void()> task);
    void Shutdown();

private:
    void WorkerLoop();

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false;
    bool joined_ = false;
};

} // namespace ss
