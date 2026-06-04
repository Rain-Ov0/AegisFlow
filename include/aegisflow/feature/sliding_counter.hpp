#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace aegisflow::feature {

template <size_t BucketNum>
class SlidingCounter {
public:
    static_assert(BucketNum > 0, "BucketNum must be greater than 0");

    explicit SlidingCounter(uint64_t bucket_ms) 
        : bucket_ms_(bucket_ms) {
            if (bucket_ms == 0) {
                throw std::invalid_argument("bucket_ms must be greater than 0");
            }
        }


    bool add(uint64_t event_ts_ms, uint64_t now_ms, uint32_t delta = 1) {
        expire(now_ms);

        const int64_t event_bucket = static_cast<int64_t>(event_ts_ms / bucket_ms_);
        const int64_t now_bucket = static_cast<int64_t>(now_ms / bucket_ms_);

        if (event_bucket > now_bucket) {
            return false;
        }

        if (event_bucket + static_cast<int64_t>(BucketNum) <= now_bucket) {
            return false;
        }

        const auto index = 
            static_cast<size_t>(event_bucket % static_cast<int64_t>(BucketNum));

        Bucket& bucket = buckets_[index];
        
        if (bucket.bucket_id != event_bucket) {
            total_ -= bucket.count;
            bucket.bucket_id = event_bucket;
            bucket.count = 0;
        }

        bucket.count += delta;
        total_ += delta;
        return true;
    }

    uint32_t sum(uint64_t now_ms) {
        expire(now_ms);
        return static_cast<uint32_t>(total_);
    }

private:
    struct Bucket {
        int64_t bucket_id = -1;
        uint32_t count = 0;
    };

    void expire(uint64_t now_ms) {
        const int64_t now_bucket = static_cast<int64_t>(now_ms / bucket_ms_);
        const int64_t min_vaild_bucket = 
            now_bucket - static_cast<int64_t>(BucketNum) + 1;

        for (Bucket& bucket : buckets_) {
            if (bucket.bucket_id != -1 && bucket.bucket_id < min_vaild_bucket) {
                total_ -= bucket.count;
                bucket.bucket_id = -1;
                bucket.count = 0;
            }
        }
        
    }

private:
    std::array<Bucket, BucketNum> buckets_{};
    uint64_t bucket_ms_;
    uint64_t total_ = 0;
};

} // namespace aegisflow::feature