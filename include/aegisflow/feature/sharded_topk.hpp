#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "aegisflow/feature/space_saving_topk.hpp"

namespace aegisflow::feature {

template <size_t ShardNum, size_t Capacity>
class ShardedTopK {
public:
    static_assert(ShardNum > 0, "ShardNum must be greater than 0");
    static_assert(Capacity > 0, "Capacity must be greater than 0");

    struct Result {
        uint64_t estimated_count = 0;
        bool in_topk = false;
    };

    Result updateAndGet(const std::string& key, uint64_t delta = 1) {
        Shard& shard = shards_[shardIndex(key)];
        
        std::lock_guard<std::mutex> lock(shard.mutex);
        
        if (delta != 0) {
            shard.topk.update(key, delta);
        }

        return {
            shard.topk.estimate(key),
            shard.topk.contains(key)
        };
    }

    void update(const std::string& key, uint64_t delta = 1) {
        Shard& shard = shards_[shardIndex(key)];
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.topk.update(key, delta);
    }

    [[nodiscard]] uint64_t estimate(const std::string& key) const {
        const Shard& shard = shards_[shardIndex(key)];
        std::lock_guard<std::mutex> lock(shard.mutex);
        return shard.topk.estimate(key);
    }

    [[nodiscard]] bool contains(const std::string& key) const {
        const Shard& shard = shards_[shardIndex(key)];
        std::lock_guard<std::mutex> lock(shard.mutex);
        return shard.topk.contains(key);
    }

    [[nodiscard]] std::vector<std::pair<std::string, uint64_t>> topk(size_t k) const {
        using Item = std::pair<std::string, uint64_t>;
        std::vector<Item> merged;

        if (k == 0) {
            return merged;
        }

        merged.reserve(k);

        for (const Shard& shard : shards_) {
            std::vector<Item> local;

            {
                std::lock_guard<std::mutex> lock(shard.mutex);
                local = shard.topk.topk(k);
            }

            if (local.empty()) {
                continue;
            }

            std::vector<Item> next;
            next.reserve(k);

            size_t i = 0;
            size_t j = 0;
            while (next.size() < k && (i < merged.size() || j < local.size())) {
                if (j >= local.size()) {
                    next.push_back(merged[i ++ ]);
                    continue;
                }
                if (i >= merged.size()) {
                    next.push_back(local[j ++ ]);
                    continue;
                }

                if (merged[i].second >= local[j].second) {
                    next.push_back(merged[i ++ ]);
                } else {
                    next.push_back(local[j ++ ]);
                }
            }

            merged = std::move(next);
        }

        while (!merged.empty() && merged.back().second == 0) {
            merged.pop_back();
        }

        return merged;
    }

private:
    struct Shard {
        mutable std::mutex mutex;
        SpaceSavingTopK topk{Capacity};
    };

    [[nodiscard]] size_t shardIndex(const std::string& key) const {
        return std::hash<std::string>{}(key) % ShardNum;
    }

private:
    std::array<Shard, ShardNum> shards_;
};

} // namespace aegisflow::feature