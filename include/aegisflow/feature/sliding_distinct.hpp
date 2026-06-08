#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace aegisflow::feature {

class SlidingDistinct {
public:
    SlidingDistinct(uint64_t window_ms, uint64_t bucket_ms, size_t max_members) {
        if (window_ms == 0) {
            throw std::invalid_argument("window_ms must be greater than 0");
        }

        if (bucket_ms == 0) {
            throw std::invalid_argument("bucket_ms must be greater than 0");
        }

        if (window_ms < bucket_ms) {
            throw std::invalid_argument("window_ms must be greater than or equal to bucket_ms");
        }

        if (max_members == 0) {
            throw std::invalid_argument("max_members must be greater than 0");
        }

        window_bucket_num_ = window_ms / bucket_ms;
        bucket_ms_ = bucket_ms;
        max_members_ = max_members;
    }

    bool add(const std::string& member, uint64_t event_ts_ms, uint64_t now_ms) {
        expire(now_ms);
        const uint64_t event_bucket = event_ts_ms / bucket_ms_;
        const uint64_t now_bucket = now_ms / bucket_ms_;

        if (event_bucket > now_bucket) {
            return false;
        }

        const uint64_t min_valid_bucket = minValidBucket(now_bucket);
        if (event_bucket < min_valid_bucket) {
            return false;
        }

        auto it = last_seen_bucket_.find(member);
        if (it != last_seen_bucket_.end()) {
            if (event_bucket <= it->second) {
                return true;
            }

            it->second = event_bucket;
            expired_queue_.emplace_back(event_bucket, member);
            return true;
        }

        if (last_seen_bucket_.size() >= max_members_) {
            degraded_ = true;
            return false;
        }

        last_seen_bucket_[member] = event_bucket;
        expired_queue_.emplace_back(event_bucket, member);
        return true;
    }

    uint32_t count(uint64_t now_ms) {
        expire(now_ms);
        return static_cast<uint32_t>(last_seen_bucket_.size());
    }

    [[nodiscard]] bool degraded() const {
        return degraded_;
    }

private:
    struct Entry {
        uint64_t bucket = 0;
        std::string member;
    };

    [[nodiscard]] uint64_t minValidBucket(uint64_t now_bucket) const {
        if (now_bucket < window_bucket_num_) {
            return 0;
        }
        return now_bucket - window_bucket_num_ + 1;
    }

    void expire(uint64_t now_ms) {
        const uint64_t now_bucket = now_ms / bucket_ms_;
        const uint64_t min_valid_bucket = minValidBucket(now_bucket);

        while (!expired_queue_.empty()) {
            const Entry& front = expired_queue_.front();

            if (front.bucket >= min_valid_bucket) {
                break;
            }

            auto it = last_seen_bucket_.find(front.member);
            if (it != last_seen_bucket_.end() && it->second == front.bucket) {
                last_seen_bucket_.erase(it);
            }

            expired_queue_.pop_front();
        }
    }

private:
    uint64_t window_bucket_num_ = 0;
    uint64_t bucket_ms_ = 0;
    size_t max_members_ = 0;

    bool degraded_ = false;

    std::unordered_map<std::string, uint64_t> last_seen_bucket_;
    std::deque<Entry> expired_queue_;
};

} // namespace aegisflow::feature
