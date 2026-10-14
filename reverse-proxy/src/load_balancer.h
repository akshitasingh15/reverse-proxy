#pragma once
// Round-robin load balancer with failover.
// Requests rotate across backends. If a backend refuses the connection,
// the next one is tried, so one dead server doesn't fail client requests.

#include <atomic>
#include <string>
#include <vector>

struct Backend {
    std::string host;
    int port;
};

class LoadBalancer {
public:
    explicit LoadBalancer(std::vector<Backend> backends) : backends_(std::move(backends)) {}

    size_t size() const { return backends_.size(); }

    // Index of the backend to try first for this request.
    // atomic fetch_add makes this thread-safe without a mutex.
    size_t next_start() { return counter_.fetch_add(1, std::memory_order_relaxed) % backends_.size(); }

    const Backend& at(size_t i) const { return backends_[i % backends_.size()]; }

private:
    std::vector<Backend> backends_;
    std::atomic<size_t> counter_{0};
};
