#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace aegisflow::feature {

template <std::size_t BucketNum>
class SlidingCounter {
public:
    static_assert(BucketNum > 0, "滑动计数器至少需要一个桶");

    explicit SlidingCounter(const std::uint64_t bucket_ms)
        : bucket_ms_(bucket_ms) {
        if (bucket_ms == 0) {
            throw std::invalid_argument("滑动计数器桶宽必须大于 0");
        }
    }

    bool add(
        const std::uint64_t event_ts_ms,
        const std::uint64_t now_ms,
        const std::uint32_t delta = 1
    ) {
        expire(now_ms);

        const auto event_bucket = event_ts_ms / bucket_ms_;
        const auto now_bucket = now_ms / bucket_ms_;
        if (event_bucket > now_bucket ||
            now_bucket - event_bucket >= BucketNum) {
            return false;
        }

        const auto index = static_cast<std::size_t>(event_bucket % BucketNum);
        Bucket& bucket = buckets_[index];
        if (!bucket.occupied || bucket.bucket_id != event_bucket) {
            total_ -= bucket.count;
            bucket.bucket_id = event_bucket;
            bucket.count = 0;
            bucket.occupied = true;
        }

        bucket.count += delta;
        total_ += delta;
        return true;
    }

    std::uint32_t sum(const std::uint64_t now_ms) {
        expire(now_ms);
        return static_cast<std::uint32_t>(total_);
    }

private:
    struct Bucket {
        std::uint64_t bucket_id = 0;
        std::uint32_t count = 0;
        bool occupied = false;
    };

    void expire(const std::uint64_t now_ms) {
        const auto now_bucket = now_ms / bucket_ms_;
        const auto minimum = now_bucket < BucketNum - 1
                                 ? 0
                                 : now_bucket - BucketNum + 1;
        for (Bucket& bucket : buckets_) {
            if (!bucket.occupied ||
                (bucket.bucket_id >= minimum &&
                 bucket.bucket_id <= now_bucket)) {
                continue;
            }
            // 时钟回拨时丢弃落在“未来”的桶，避免风险计数被旧时间线放大。
            total_ -= bucket.count;
            bucket = {};
        }
    }

    std::array<Bucket, BucketNum> buckets_{};
    std::uint64_t bucket_ms_;
    std::uint64_t total_ = 0;
};

}  // 命名空间 aegisflow::feature
