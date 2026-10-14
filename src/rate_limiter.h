#pragma once
// Per-client token-bucket rate limiter.
// Each client IP gets a bucket holding up to `capacity` tokens, refilled at
// `refill_per_sec`. Every request spends one token; an empty bucket means 429.
// This allows short bursts (up to capacity) while capping the long-run rate.

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

class RateLimiter {
public:
    RateLimiter(double capacity, double refill_per_sec)
        : capacity_(capacity), refill_per_sec_(refill_per_sec) {}

    bool allow(const std::string& client_ip) {
        if (capacity_ <= 0) return true;  // rate limiting disabled
        auto now = Clock::now();

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = buckets_.find(client_ip);
        if (it == buckets_.end()) {
            it = buckets_.emplace(client_ip, Bucket{capacity_, now}).first;
        }
        Bucket& b = it->second;

        // Refill lazily based on time elapsed since the last request,
        // so no background timer thread is needed.
        double elapsed = std::chrono::duration<double>(now - b.last_refill).count();
        b.tokens = std::min(capacity_, b.tokens + elapsed * refill_per_sec_);
        b.last_refill = now;

        if (b.tokens >= 1.0) {
            b.tokens -= 1.0;
            return true;
        }
        return false;
    }

private:
    using Clock = std::chrono::steady_clock;  // steady: unaffected by system clock changes
    struct Bucket {
        double tokens;
        Clock::time_point last_refill;
    };

    double capacity_;
    double refill_per_sec_;
    std::unordered_map<std::string, Bucket> buckets_;
    std::mutex mutex_;
};
