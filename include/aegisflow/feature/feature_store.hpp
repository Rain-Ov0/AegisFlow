#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "event.pb.h"
#include "aegisflow/feature/user_state.hpp"
#include "aegisflow/feature/sharded_topk.hpp"
#include "aegisflow/feature/sliding_distinct.hpp"
#include "aegisflow/feature/sharded_count_min_sketch.hpp"


namespace aegisflow::feature {

class FeatureStore {

public:
    FeatureSnapshot updateAndGet(
        const aegisflow::v1::Event& event,
        uint64_t now_ms
    );

private:
    static constexpr size_t kShardNum = 64;
    static constexpr uint64_t kMaxWindows = 60ULL * 60ULL * 1000ULL;

    static constexpr uint64_t kDistinctWindowMs = 10ULL * 60ULL * 1000ULL;
    static constexpr uint64_t kDistinctBucketMs = 10ULL * 1000ULL;
    static constexpr size_t kDistinctMaxMembers = 5000;

    static constexpr size_t kTopKShardNum = 16;
    static constexpr size_t kTopKCapacity = 100;

    static constexpr size_t kCmsShardNum = 16;
    static constexpr size_t kCmsDepth = 4;
    static constexpr size_t kCmsWidth = 16384;

    static constexpr uint64_t kTopKMinEstimatedCount = 20;

    struct Shard {
        std::mutex mutex;
        std::unordered_map<std::string, UserState> users;
    };

    struct DistinctShard {
        std::mutex mutex;
        std::unordered_map<std::string, SlidingDistinct> states;
    };

    void updateIpDistinct(
        const aegisflow::v1::Event& event,
        uint64_t now_ms,
        FeatureSnapshot& out
    );

    void updateDeviceDistinct(
        const aegisflow::v1::Event& event,
        uint64_t now_ms,
        FeatureSnapshot& out
    );

    void updateTopK(
        const aegisflow::v1::Event& event,
        FeatureSnapshot& out
    );

    void updateCms(
        const aegisflow::v1::Event& event,
        FeatureSnapshot& out
    );

    [[nodiscard]] size_t shardIndex(const std::string& user_id) const;

    [[nodiscard]] static bool isEventTimeValid(uint64_t event_ts_ms, uint64_t now_ms);
    
    static FeatureSnapshot buildSnapshot(
        const std::string& user_id,
        UserState& state,
        uint64_t now_ms
    );

    [[nodiscard]] static std::string buildRiskKey(
        const aegisflow::v1::Event& event
    );

private:
    std::array<Shard, kShardNum> shards_;
    std::array<DistinctShard, kShardNum> ip_shards_;
    std::array<DistinctShard, kShardNum> device_shards_;

    ShardedTopK<kTopKShardNum, kTopKCapacity> ip_topk_;
    ShardedCountMinSketch<kCmsShardNum, kCmsDepth, kCmsWidth> risk_cms_;
};

}