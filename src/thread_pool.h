#pragma once
// Fixed-size thread pool with a bounded job queue.
// The accept loop pushes client sockets; worker threads pop and handle them.
// If the queue is full, submit() returns false so the caller can shed load (503)
// instead of letting memory grow without limit.

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class ThreadPool {
public:
    ThreadPool(size_t num_threads, size_t max_queue) : max_queue_(max_queue) {
        for (size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }

    bool submit(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (jobs_.size() >= max_queue_) return false;
            jobs_.push(std::move(job));
        }
        cv_.notify_one();  // wake exactly one sleeping worker
        return true;
    }

private:
    void worker_loop() {
        while (true) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // Sleep until there is work or we are shutting down (no busy-waiting).
                cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                if (stopping_ && jobs_.empty()) return;
                job = std::move(jobs_.front());
                jobs_.pop();
            }
            job();  // run outside the lock so other workers can grab jobs
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> jobs_;
    std::mutex mutex_;
    std::condition_variable cv_;
    size_t max_queue_;
    bool stopping_ = false;
};
