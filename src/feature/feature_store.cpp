#include "aegisflow/feature/feature_store.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>

namespace aegisflow::feature {

LoginFeatureStore::LoginFeatureStore()
    : LoginFeatureStore(FeatureStateReclamationConfig{}) {}

LoginFeatureStore::LoginFeatureStore(FeatureStateReclamationConfig config)
    : reclamation_config_(config) {
    validateReclamationConfig(reclamation_config_);
}

LoginFeatureSnapshot LoginFeatureStore::updateAndGet(
    const aegisflow::domain::LoginAttempt& attempt,
    const std::uint64_t now_ms
) {
    // user/IP/device 各自分片加锁，一次请求不同时持有两把锁。
    LoginFeatureSnapshot snapshot;
    updateUser(attempt, now_ms, snapshot);
    updateIp(attempt, now_ms, snapshot);
    updateDevice(attempt, now_ms, snapshot);

    return snapshot;
}

void LoginFeatureStore::updateUser(
    const aegisflow::domain::LoginAttempt& attempt,
    const std::uint64_t now_ms,
    LoginFeatureSnapshot& out
) {
    const bool can_update_failure =
        attempt.result() == aegisflow::domain::LoginResult::fail &&
        isWithinWindow(
            attempt.timestampMs(),
            now_ms,
            5ULL * 60ULL * 1000ULL
        );

    // 成功登录可读取已有失败窗口，但不创建或延长失败状态。
    UserShard& shard = user_shards_[
        shardIndex(attempt.userId(), kUserShardNum)
    ];
    std::lock_guard<std::mutex> lock(shard.mutex);

    std::string key(attempt.userId());
    auto it = shard.users.find(key);
    if (it == shard.users.end() && can_update_failure) {
        it = shard.users.try_emplace(std::move(key)).first;
    }
    if (it == shard.users.end()) {
        return;
    }

    LoginUserState& state = it->second;
    if (can_update_failure) {
        state.login_fail_5m.add(attempt.timestampMs(), now_ms);
        state.last_seen_ms = now_ms;
    }

    buildUserSnapshot(state, now_ms, out);
}

void LoginFeatureStore::updateIp(
    const aegisflow::domain::LoginAttempt& attempt,
    const std::uint64_t now_ms,
    LoginFeatureSnapshot& out
) {
    IpShard& shard = ip_shards_[shardIndex(attempt.ip(), kDistinctShardNum)];
    std::lock_guard<std::mutex> lock(shard.mutex);

    std::string key(attempt.ip());
    auto it = shard.states.find(key);
    const bool can_update_failure =
        attempt.result() == aegisflow::domain::LoginResult::fail &&
        isWithinWindow(attempt.timestampMs(), now_ms, kDistinctWindowMs);
    // IP distinct 的成员是“失败用户”；成功请求不得污染扫号特征。
    if (it == shard.states.end() && can_update_failure) {
        it = shard.states.try_emplace(
            std::move(key),
            kDistinctWindowMs,
            kDistinctBucketMs,
            kDistinctMaxMembers
        ).first;
    }
    if (it == shard.states.end()) {
        return;
    }

    IpFeatureState& state = it->second;
    SlidingDistinct& distinct = state.distinct;
    if (can_update_failure) {
        (void)distinct.add(
            attempt.userId(), attempt.timestampMs(), now_ms);
        state.failures_10m.add(attempt.timestampMs(), now_ms);
        state.last_seen_ms = now_ms;
    }
    out.ip_distinct_failed_user_10m = distinct.count(now_ms);
    out.ip_login_failures_10m = state.failures_10m.sum(now_ms);
}

void LoginFeatureStore::updateDevice(
    const aegisflow::domain::LoginAttempt& attempt,
    const std::uint64_t now_ms,
    LoginFeatureSnapshot& out
) {
    DeviceShard& shard = device_shards_[
        shardIndex(attempt.deviceId(), kDistinctShardNum)
    ];
    std::lock_guard<std::mutex> lock(shard.mutex);

    std::string key(attempt.deviceId());
    auto it = shard.states.find(key);
    if (it == shard.states.end() &&
        isWithinWindow(attempt.timestampMs(), now_ms, kDistinctWindowMs)) {
        it = shard.states.try_emplace(
            std::move(key),
            kDistinctWindowMs,
            kDistinctBucketMs,
            kDistinctMaxMembers
        ).first;
    }
    if (it == shard.states.end()) {
        return;
    }

    DeviceFeatureState& state = it->second;
    SlidingDistinct& distinct = state.distinct;
    if (isWithinWindow(
            attempt.timestampMs(), now_ms, kDistinctWindowMs)) {
        (void)distinct.add(
            attempt.userId(), attempt.timestampMs(), now_ms);
        state.last_seen_ms = now_ms;
    }
    out.device_distinct_account_10m = distinct.count(now_ms);
}

bool LoginFeatureStore::isWithinWindow(
    const std::uint64_t event_ts_ms,
    const std::uint64_t now_ms,
    const std::uint64_t window_ms
) noexcept {
    return event_ts_ms <= now_ms && now_ms - event_ts_ms < window_ms;
}

std::size_t LoginFeatureStore::shardIndex(
    const std::string& key,
    const std::size_t shard_num
) {
    return std::hash<std::string>{}(key) % shard_num;
}

void LoginFeatureStore::buildUserSnapshot(
    LoginUserState& state,
    const std::uint64_t now_ms,
    LoginFeatureSnapshot& out
) {
    out.user_login_fail_5m = state.login_fail_5m.sum(now_ms);
}

bool LoginFeatureStore::isValidReclamationConfig(
    const FeatureStateReclamationConfig& config
) noexcept {
    constexpr std::uint64_t kUserMinimumTtlMs = 5ULL * 60ULL * 1000ULL;
    return config.user_ttl_ms >= kUserMinimumTtlMs &&
           config.ip_ttl_ms >= kDistinctWindowMs &&
           config.device_ttl_ms >= kDistinctWindowMs;
}

void LoginFeatureStore::reclaimColdStates(
    const std::uint64_t now_ms
) {
    // map 是状态的唯一所有者：每次回收在一把分片锁内直接遍历全部元素，
    // 删除时不需要同步第二套扫描顺序或成员计数。锁在每个循环体末尾释放，
    // 因此回收始终只持有一把 shard 锁，不与请求更新形成多锁顺序。
    for (auto& shard : user_shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        for (auto position = shard.users.begin();
             position != shard.users.end();) {
            if (isExpired(
                    position->second.last_seen_ms,
                    now_ms,
                    reclamation_config_.user_ttl_ms)) {
                position = shard.users.erase(position);
            } else {
                ++position;
            }
        }
    }
    for (auto& shard : ip_shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        for (auto position = shard.states.begin();
             position != shard.states.end();) {
            if (isExpired(
                    position->second.last_seen_ms,
                    now_ms,
                    reclamation_config_.ip_ttl_ms)) {
                position = shard.states.erase(position);
            } else {
                ++position;
            }
        }
    }
    for (auto& shard : device_shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        for (auto position = shard.states.begin();
             position != shard.states.end();) {
            if (isExpired(
                    position->second.last_seen_ms,
                    now_ms,
                    reclamation_config_.device_ttl_ms)) {
                position = shard.states.erase(position);
            } else {
                ++position;
            }
        }
    }
}

FeatureStoreStats LoginFeatureStore::currentStats() const {
    FeatureStoreStats stats;

    for (const auto& shard : user_shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        stats.user_state_count += shard.users.size();
    }
    for (const auto& shard : ip_shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        stats.ip_state_count += shard.states.size();
        for (const auto& entry : shard.states) {
            stats.ip_distinct_member_count +=
                entry.second.distinct.activeMemberCount();
        }
    }
    for (const auto& shard : device_shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        stats.device_state_count += shard.states.size();
        for (const auto& entry : shard.states) {
            stats.device_distinct_member_count +=
                entry.second.distinct.activeMemberCount();
        }
    }

    return stats;
}

void LoginFeatureStore::validateReclamationConfig(
    const FeatureStateReclamationConfig& config
) {
    constexpr std::uint64_t kUserMinimumTtlMs = 5ULL * 60ULL * 1000ULL;
    if (config.user_ttl_ms < kUserMinimumTtlMs) {
        throw std::invalid_argument("用户状态 TTL 不得短于最长用户窗口");
    }
    if (config.ip_ttl_ms < kDistinctWindowMs ||
        config.device_ttl_ms < kDistinctWindowMs) {
        throw std::invalid_argument("distinct 状态 TTL 不得短于窗口");
    }
}

bool LoginFeatureStore::isExpired(
    const std::uint64_t last_seen_ms,
    const std::uint64_t now_ms,
    const std::uint64_t ttl_ms
) noexcept {
    return now_ms >= last_seen_ms && now_ms - last_seen_ms >= ttl_ms;
}

}  // 命名空间 aegisflow::feature
