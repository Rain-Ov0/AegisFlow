#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string_view>

#include "aegisflow/feature/count_min_sketch.hpp"

namespace aegisflow::feature {

template <size_t ShardNum, size_t Depth, size_t TotalWidth>
class ShardedCountMinSketch {
public:
    static_assert(ShardNum > 0, "ShardNum must be greater than 0");
    static_assert(Depth > 0, "Depth must be greater than 0");
    static_assert(TotalWidth > 0, "TotalWidth must be greater than 0");

    void add(std::string_view key, uint32_t delta = 1) {
        Shard& shard = shards_[shardIndex(key)];
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.cms.add(key, delta);
    }

    [[nodiscard]] uint64_t estimate(std::string_view key) const {
        const Shard& shard = shards_[shardIndex(key)];
        std::lock_guard<std::mutex> lock(shard.mutex);
        return shard.cms.estimate(key);
    }

    uint64_t addAndEstimate(std::string_view key, uint32_t delta = 1) {
        Shard& shard = shards_[shardIndex(key)];
        std::lock_guard<std::mutex> lock(shard.mutex);

        if (delta != 0) {
            shard.cms.add(key, delta);
        }

        return shard.cms.estimate(key);
    }

    void reset() {
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.cms.reset();
        }
    }

private:
    static constexpr size_t kShardWidth = 
        (TotalWidth + ShardNum - 1) / ShardNum;

    struct Shard {
        mutable std::mutex mutex;
        CountMinSketch cms{Depth, kShardWidth};
    };

    [[nodiscard]] size_t shardIndex(std::string_view key) const {
        return std::hash<std::string_view>()(key) % ShardNum;
    }
private:

    std::array<Shard, ShardNum> shards_;
};

} // namespace aegisflow::feature