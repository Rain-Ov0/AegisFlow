#pragma once

#include "aegisflow/domain/login.hpp"
#include "aegisflow/feature/sliding_counter.hpp"
#include "aegisflow/feature/sliding_distinct.hpp"
#include "aegisflow/feature/user_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace aegisflow::feature {

struct FeatureStateReclamationConfig {
    std::uint64_t user_ttl_ms = 10ULL * 60ULL * 1000ULL;
    std::uint64_t ip_ttl_ms = 20ULL * 60ULL * 1000ULL;
    std::uint64_t device_ttl_ms = 20ULL * 60ULL * 1000ULL;

    bool operator==(const FeatureStateReclamationConfig& other) const {
        return user_ttl_ms == other.user_ttl_ms &&
               ip_ttl_ms == other.ip_ttl_ms &&
               device_ttl_ms == other.device_ttl_ms;
    }
};

struct FeatureStoreStats {
    std::uint64_t user_state_count = 0;
    std::uint64_t ip_state_count = 0;
    std::uint64_t ip_distinct_member_count = 0;
    std::uint64_t device_state_count = 0;
    std::uint64_t device_distinct_member_count = 0;
};

class LoginFeatureStore final {
public:
    static constexpr std::size_t kUserShardNum = 64;
    static constexpr std::size_t kDistinctShardNum = 64;
    static constexpr std::uint64_t kDistinctWindowMs =
        10ULL * 60ULL * 1000ULL;
    static constexpr std::uint64_t kDistinctBucketMs = 10ULL * 1000ULL;
    static constexpr std::size_t kDistinctMaxMembers = 5000;
    static constexpr std::uint64_t kIpFailureBucketMs = 10ULL * 1000ULL;

    LoginFeatureStore();
    explicit LoginFeatureStore(FeatureStateReclamationConfig config);

    [[nodiscard]] LoginFeatureSnapshot updateAndGet(
        const aegisflow::domain::LoginAttempt& attempt,
        std::uint64_t now_ms
    );

    [[nodiscard]] static bool isValidReclamationConfig(
        const FeatureStateReclamationConfig& config
    ) noexcept;

    void reclaimColdStates(std::uint64_t now_ms);

    // 按需在分片锁内汇总当前容器，不让登录热路径维护镜像计数。
    // 该方法没有业务时间参数，因此不用系统时钟隐式推进滑动窗口。
    [[nodiscard]] FeatureStoreStats currentStats() const;

private:
    struct IpFeatureState {
        IpFeatureState(
            std::uint64_t window_ms,
            std::uint64_t bucket_ms,
            std::size_t max_members
        ) : distinct(window_ms, bucket_ms, max_members) {}

        SlidingDistinct distinct;
        SlidingCounter<60> failures_10m{kIpFailureBucketMs};
        std::uint64_t last_seen_ms = 0;
    };

    struct DeviceFeatureState {
        DeviceFeatureState(
            std::uint64_t window_ms,
            std::uint64_t bucket_ms,
            std::size_t max_members
        ) : distinct(window_ms, bucket_ms, max_members) {}

        SlidingDistinct distinct;
        std::uint64_t last_seen_ms = 0;
    };

    using UserMap = std::unordered_map<std::string, LoginUserState>;
    using IpMap = std::unordered_map<std::string, IpFeatureState>;
    using DeviceMap = std::unordered_map<std::string, DeviceFeatureState>;

    struct UserShard {
        mutable std::mutex mutex;
        UserMap users;
    };

    struct IpShard {
        mutable std::mutex mutex;
        IpMap states;
    };

    struct DeviceShard {
        mutable std::mutex mutex;
        DeviceMap states;
    };

    void updateUser(
        const aegisflow::domain::LoginAttempt& attempt,
        std::uint64_t now_ms,
        LoginFeatureSnapshot& out
    );

    void updateIp(
        const aegisflow::domain::LoginAttempt& attempt,
        std::uint64_t now_ms,
        LoginFeatureSnapshot& out
    );

    void updateDevice(
        const aegisflow::domain::LoginAttempt& attempt,
        std::uint64_t now_ms,
        LoginFeatureSnapshot& out
    );

    [[nodiscard]] static bool isWithinWindow(
        std::uint64_t event_ts_ms,
        std::uint64_t now_ms,
        std::uint64_t window_ms
    ) noexcept;

    [[nodiscard]] static std::size_t shardIndex(
        const std::string& key,
        std::size_t shard_num
    );

    static void buildUserSnapshot(
        LoginUserState& state,
        std::uint64_t now_ms,
        LoginFeatureSnapshot& out
    );

    static void validateReclamationConfig(
        const FeatureStateReclamationConfig& config
    );

    [[nodiscard]] static bool isExpired(
        std::uint64_t last_seen_ms,
        std::uint64_t now_ms,
        std::uint64_t ttl_ms
    ) noexcept;

    FeatureStateReclamationConfig reclamation_config_;
    std::array<UserShard, kUserShardNum> user_shards_;
    std::array<IpShard, kDistinctShardNum> ip_shards_;
    std::array<DeviceShard, kDistinctShardNum> device_shards_;
};

}  // 命名空间 aegisflow::feature
