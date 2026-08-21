#include "aegisflow/app/login_request_validator.hpp"
#include "aegisflow/feature/feature_store.hpp"

#include "tests/support/test_harness.hpp"

#include "login.pb.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using aegisflow::test::require;

aegisflow::domain::LoginAttempt attemptFor(
    const std::uint64_t now_ms,
    const std::uint64_t attempt_id,
    const std::string_view user_id,
    const std::string_view ip,
    const std::string_view device_id,
    const aegisflow::login::LoginResult result
) {
    aegisflow::login::LoginRequest request;
    request.set_attempt_id(attempt_id);
    request.set_timestamp_ms(now_ms);
    request.set_user_id(user_id.data(), user_id.size());
    request.set_ip(ip.data(), ip.size());
    request.set_device_id(device_id.data(), device_id.size());
    request.set_result(result);
    auto attempt = aegisflow::app::LoginRequestValidator::validate(
        request,
        now_ms
    );
    require(attempt.has_value(), "FeatureStore 测试请求必须通过校验");
    return std::move(*attempt);
}

void failedIpDistinctExcludesSuccessfulUsers() {
    constexpr std::uint64_t now_ms = 10'000'000;
    constexpr std::string_view ip = "203.0.113.7";
    aegisflow::feature::LoginFeatureStore store;

    auto snapshot = store.updateAndGet(
        attemptFor(now_ms, 1, "success-a", ip, "device-a",
                   aegisflow::login::SUCCESS),
        now_ms
    );
    snapshot = store.updateAndGet(
        attemptFor(now_ms, 2, "success-b", ip, "device-b",
                   aegisflow::login::SUCCESS),
        now_ms
    );
    require(
        snapshot.ip_distinct_failed_user_10m == 0 &&
            snapshot.ip_login_failures_10m == 0,
        "成功登录不得进入 IP 失败用户特征"
    );

    snapshot = store.updateAndGet(
        attemptFor(now_ms, 3, "failed-a", ip, "device-c",
                   aegisflow::login::FAIL),
        now_ms
    );
    snapshot = store.updateAndGet(
        attemptFor(now_ms, 4, "failed-b", ip, "device-d",
                   aegisflow::login::FAIL),
        now_ms
    );
    require(
        snapshot.ip_distinct_failed_user_10m == 2 &&
            snapshot.ip_login_failures_10m == 2,
        "不同失败用户必须形成准确 distinct 和失败计数"
    );
}

void userIpAndDeviceWindowsExpireWithoutCrossContamination() {
    constexpr std::uint64_t first_ms = 10'000'000;
    aegisflow::feature::LoginFeatureStore store;
    auto snapshot = store.updateAndGet(
        attemptFor(first_ms, 1, "user-a", "203.0.113.8", "device-a",
                   aegisflow::login::FAIL),
        first_ms
    );
    require(
        snapshot.user_login_fail_5m == 1 &&
            snapshot.ip_distinct_failed_user_10m == 1 &&
            snapshot.ip_login_failures_10m == 1 &&
            snapshot.device_distinct_account_10m == 1,
        "首次失败必须同时更新四项滑动特征"
    );

    constexpr std::uint64_t user_expired_ms = first_ms + 5ULL * 60ULL * 1000ULL;
    snapshot = store.updateAndGet(
        attemptFor(user_expired_ms, 2, "user-a", "203.0.113.9", "device-b",
                   aegisflow::login::SUCCESS),
        user_expired_ms
    );
    require(snapshot.user_login_fail_5m == 0, "用户失败特征必须在 5 分钟边界过期");

    constexpr std::uint64_t distinct_expired_ms =
        first_ms + aegisflow::feature::LoginFeatureStore::kDistinctWindowMs;
    snapshot = store.updateAndGet(
        attemptFor(distinct_expired_ms, 3, "user-b", "203.0.113.8", "device-a",
                   aegisflow::login::SUCCESS),
        distinct_expired_ms
    );
    require(
        snapshot.ip_distinct_failed_user_10m == 0 &&
            snapshot.ip_login_failures_10m == 0,
        "IP distinct 与失败计数必须在 10 分钟边界过期"
    );
    require(
        snapshot.device_distinct_account_10m == 1,
        "当前请求应作为新窗口的唯一设备成员"
    );
}

void coldStatesAreReclaimedAndStatsFollowOwnership() {
    using aegisflow::feature::FeatureStateReclamationConfig;
    using aegisflow::feature::LoginFeatureStore;

    FeatureStateReclamationConfig config;
    config.user_ttl_ms = 5ULL * 60ULL * 1000ULL;
    config.ip_ttl_ms = LoginFeatureStore::kDistinctWindowMs;
    config.device_ttl_ms = LoginFeatureStore::kDistinctWindowMs;
    LoginFeatureStore store(config);

    constexpr std::uint64_t first_ms = 10'000'000;
    static_cast<void>(store.updateAndGet(
        attemptFor(first_ms, 1, "user-a", "203.0.113.10", "device-a",
                   aegisflow::login::FAIL),
        first_ms
    ));
    auto stats = store.currentStats();
    require(
        stats.user_state_count == 1 && stats.ip_state_count == 1 &&
            stats.ip_distinct_member_count == 1 &&
            stats.device_state_count == 1 &&
            stats.device_distinct_member_count == 1,
        "状态统计必须与 user/IP/device 所有权一致"
    );

    store.reclaimColdStates(first_ms + LoginFeatureStore::kDistinctWindowMs);
    stats = store.currentStats();
    require(
        stats.user_state_count == 0 && stats.ip_state_count == 0 &&
            stats.ip_distinct_member_count == 0 &&
            stats.device_state_count == 0 &&
            stats.device_distinct_member_count == 0,
        "超过 TTL 的冷状态及 distinct 成员必须被完整回收"
    );
}

std::vector<std::string> usersInOneShard(const std::size_t count) {
    std::vector<std::string> users;
    users.reserve(count);
    std::size_t target_shard = 0;
    for (std::size_t candidate = 0; users.size() < count; ++candidate) {
        std::string user = "same-shard-user-" + std::to_string(candidate);
        const auto shard = std::hash<std::string>{}(user) %
                           aegisflow::feature::LoginFeatureStore::kUserShardNum;
        if (users.empty()) {
            target_shard = shard;
        }
        if (shard == target_shard) {
            users.push_back(std::move(user));
        }
    }
    return users;
}

void onePassReclaimsEveryExpiredStateInLargeShard() {
    using aegisflow::feature::FeatureStateReclamationConfig;
    using aegisflow::feature::LoginFeatureStore;

    FeatureStateReclamationConfig config;
    config.user_ttl_ms = 5ULL * 60ULL * 1000ULL;
    config.ip_ttl_ms = LoginFeatureStore::kDistinctWindowMs;
    config.device_ttl_ms = LoginFeatureStore::kDistinctWindowMs;
    LoginFeatureStore store(config);

    constexpr std::uint64_t first_ms = 20'000'000;
    const auto users = usersInOneShard(96);
    for (std::size_t index = 0; index < users.size(); ++index) {
        static_cast<void>(store.updateAndGet(
            attemptFor(
                first_ms,
                index + 1,
                users[index],
                "198.51.100.20",
                "large-shard-device",
                aegisflow::login::FAIL
            ),
            first_ms
        ));
    }
    require(
        store.currentStats().user_state_count == users.size(),
        "大分片必须先真实容纳超过旧扫描预算的状态"
    );

    store.reclaimColdStates(first_ms + LoginFeatureStore::kDistinctWindowMs);
    const auto stats = store.currentStats();
    require(
        stats.user_state_count == 0 && stats.ip_state_count == 0 &&
            stats.device_state_count == 0,
        "一次直接扫描必须回收同一分片内全部 96 个过期状态"
    );
}

void statsAggregateDistinctMembersOnDemand() {
    constexpr std::uint64_t now_ms = 30'000'000;
    aegisflow::feature::LoginFeatureStore store;

    static_cast<void>(store.updateAndGet(
        attemptFor(now_ms, 1, "user-a", "198.51.100.31", "device-a",
                   aegisflow::login::FAIL),
        now_ms
    ));
    static_cast<void>(store.updateAndGet(
        attemptFor(now_ms, 2, "user-b", "198.51.100.31", "device-a",
                   aegisflow::login::SUCCESS),
        now_ms
    ));
    static_cast<void>(store.updateAndGet(
        attemptFor(now_ms, 3, "user-c", "198.51.100.31", "device-b",
                   aegisflow::login::FAIL),
        now_ms
    ));
    static_cast<void>(store.updateAndGet(
        attemptFor(now_ms, 4, "user-a", "198.51.100.32", "device-a",
                   aegisflow::login::FAIL),
        now_ms
    ));

    const auto stats = store.currentStats();
    require(
        stats.ip_state_count == 2 && stats.ip_distinct_member_count == 3,
        "IP distinct 成员数必须在查询时逐状态汇总"
    );
    require(
        stats.device_state_count == 2 &&
            stats.device_distinct_member_count == 3,
        "device distinct 成员数必须在查询时逐状态汇总"
    );
}

void successDoesNotExtendFailureStateButDoesExtendDeviceState() {
    using aegisflow::feature::FeatureStateReclamationConfig;
    using aegisflow::feature::LoginFeatureStore;

    FeatureStateReclamationConfig config;
    config.user_ttl_ms = 5ULL * 60ULL * 1000ULL;
    config.ip_ttl_ms = LoginFeatureStore::kDistinctWindowMs;
    config.device_ttl_ms = LoginFeatureStore::kDistinctWindowMs;
    LoginFeatureStore store(config);

    constexpr std::uint64_t first_ms = 35'000'000;
    constexpr std::uint64_t before_ip_ttl =
        first_ms + LoginFeatureStore::kDistinctWindowMs - 1;
    static_cast<void>(store.updateAndGet(
        attemptFor(first_ms, 1, "ttl-user", "198.51.100.35", "ttl-device",
                   aegisflow::login::FAIL),
        first_ms
    ));
    static_cast<void>(store.updateAndGet(
        attemptFor(before_ip_ttl, 2, "ttl-user", "198.51.100.35",
                   "ttl-device", aegisflow::login::SUCCESS),
        before_ip_ttl
    ));

    store.reclaimColdStates(
        first_ms + LoginFeatureStore::kDistinctWindowMs);
    const auto stats = store.currentStats();
    require(
        stats.user_state_count == 0 && stats.ip_state_count == 0,
        "成功登录不得续期用户或 IP 失败状态"
    );
    require(
        stats.device_state_count == 1 &&
            stats.device_distinct_member_count == 1,
        "device 状态必须由成功和失败登录共同更新与续期"
    );
}

void unexpiredStatesSurviveConcurrentUpdatesAndReclamation() {
    using aegisflow::feature::FeatureStateReclamationConfig;
    using aegisflow::feature::LoginFeatureStore;

    FeatureStateReclamationConfig config;
    config.user_ttl_ms = 5ULL * 60ULL * 1000ULL;
    config.ip_ttl_ms = LoginFeatureStore::kDistinctWindowMs;
    config.device_ttl_ms = LoginFeatureStore::kDistinctWindowMs;
    LoginFeatureStore store(config);

    constexpr std::uint64_t first_ms = 40'000'000;
    constexpr std::uint64_t update_ms = first_ms + 100;
    std::vector<aegisflow::domain::LoginAttempt> attempts;
    attempts.reserve(32);
    for (std::size_t index = 0; index < 32; ++index) {
        const auto ip = "198.51.100." + std::to_string(100 + index);
        const auto user = "concurrent-user-" + std::to_string(index);
        const auto device = "concurrent-device-" + std::to_string(index);
        static_cast<void>(store.updateAndGet(
            attemptFor(
                first_ms,
                index + 1,
                user,
                ip,
                device,
                aegisflow::login::FAIL
            ),
            first_ms
        ));
        attempts.push_back(attemptFor(
            update_ms,
            1000 + index,
            user,
            ip,
            device,
            aegisflow::login::FAIL
        ));
    }

    std::atomic<bool> start = false;
    std::atomic<bool> failed = false;
    std::vector<std::thread> threads;
    for (std::size_t worker = 0; worker < 4; ++worker) {
        threads.emplace_back([&, worker] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            try {
                for (std::size_t round = 0; round < 100; ++round) {
                    for (std::size_t index = worker;
                         index < attempts.size();
                         index += 4) {
                        static_cast<void>(
                            store.updateAndGet(attempts[index], update_ms));
                    }
                }
            } catch (...) {
                failed.store(true, std::memory_order_relaxed);
            }
        });
    }
    threads.emplace_back([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        try {
            for (std::size_t round = 0; round < 200; ++round) {
                store.reclaimColdStates(
                    first_ms + config.user_ttl_ms - 1);
            }
        } catch (...) {
            failed.store(true, std::memory_order_relaxed);
        }
    });
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }

    const auto stats = store.currentStats();
    require(!failed.load(), "并发更新与回收不得抛出异常");
    require(
        stats.user_state_count == 32 && stats.ip_state_count == 32 &&
            stats.device_state_count == 32 &&
            stats.ip_distinct_member_count == 32 &&
            stats.device_distinct_member_count == 32,
        "未到 TTL 的状态在并发更新与直接回收中不得丢失"
    );
}

void clockRollbackDoesNotDeleteStates() {
    constexpr std::uint64_t first_ms = 50'000'000;
    aegisflow::feature::LoginFeatureStore store;
    static_cast<void>(store.updateAndGet(
        attemptFor(first_ms, 1, "rollback-user", "198.51.100.50",
                   "rollback-device", aegisflow::login::FAIL),
        first_ms
    ));

    store.reclaimColdStates(first_ms - 1);
    const auto stats = store.currentStats();
    require(
        stats.user_state_count == 1 && stats.ip_state_count == 1 &&
            stats.device_state_count == 1,
        "时钟回拨时不得将未来 last_seen 误判为过期"
    );
}

void invalidReclamationConfigurationIsRejected() {
    auto config = aegisflow::feature::FeatureStateReclamationConfig{};
    config.user_ttl_ms = 5ULL * 60ULL * 1000ULL - 1;
    require(
        !aegisflow::feature::LoginFeatureStore::isValidReclamationConfig(config),
        "短于用户窗口的 TTL 必须被拒绝"
    );

    config = {};
    config.ip_ttl_ms =
        aegisflow::feature::LoginFeatureStore::kDistinctWindowMs - 1;
    require(
        !aegisflow::feature::LoginFeatureStore::isValidReclamationConfig(config),
        "IP 状态 TTL 不得短于 distinct 窗口"
    );
}

}  // namespace

int main() {
    return aegisflow::test::runModule(
        "feature_store",
        {
            {"IP distinct 只统计失败用户", failedIpDistinctExcludesSuccessfulUsers},
            {"特征窗口过期", userIpAndDeviceWindowsExpireWithoutCrossContamination},
            {"冷状态回收", coldStatesAreReclaimedAndStatsFollowOwnership},
            {"大分片一次完整回收", onePassReclaimsEveryExpiredStateInLargeShard},
            {"distinct 成员按需汇总", statsAggregateDistinctMembersOnDemand},
            {"成功登录的状态续期边界", successDoesNotExtendFailureStateButDoesExtendDeviceState},
            {"并发更新与回收", unexpiredStatesSurviveConcurrentUpdatesAndReclamation},
            {"时钟回拨不回收", clockRollbackDoesNotDeleteStates},
            {"回收配置不变式", invalidReclamationConfigurationIsRejected},
        }
    );
}
