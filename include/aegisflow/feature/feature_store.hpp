#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "event.pb.h"
#include "aegisflow/feature/user_state.hpp"

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

    struct Shard {
        std::mutex mutex;
        std::unordered_map<std::string, UserState> users;
    };

    [[nodiscard]] size_t shardIndex(const std::string& user_id) const;
    [[nodiscard]] static bool isEventTimeValid(uint64_t event_ts_ms, uint64_t now_ms);
    static FeatureSnapshot buildSnapshot(
        const std::string& user_id,
        UserState& state,
        uint64_t now_ms
    );

private:
    std::array<Shard, kShardNum> shards_;
};

}