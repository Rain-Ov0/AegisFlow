#include "aegisflow/feature/feature_store.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace v1 = aegisflow::v1;

using aegisflow::feature::FeatureSnapshot;
using aegisflow::feature::FeatureStore;

v1::Event makeEvent(
    uint64_t event_id,
    const std::string& user_id,
    uint64_t timestamp_ms,
    v1::EventType type,
    v1::EventResult result,
    const std::string& ip,
    const std::string& device_id,
    const std::string& scene
) {
    v1::Event event;
    event.set_event_id(event_id);
    event.set_user_id(user_id);
    event.set_timestamp_ms(timestamp_ms);
    event.set_type(type);
    event.set_result(result);
    event.set_ip(ip);
    event.set_device_id(device_id);
    event.set_scene(scene);
    return event;
}

void test_ip_distinct_counts_unique_users() {
    FeatureStore store;

    constexpr uint64_t kBaseTsMs = 1000000;
    const std::string ip = "203.0.113.1";

    FeatureSnapshot snapshot;

    for (uint64_t i = 0; i < 20; ++i) {
        snapshot = store.updateAndGet(
            makeEvent(
                i,
                "user_" + std::to_string(i),
                kBaseTsMs + i,
                v1::LOGIN,
                v1::SUCCESS,
                ip,
                "device_" + std::to_string(i),
                "login"
            ),
            kBaseTsMs + i
        );
    }

    assert(snapshot.ip_distinct_user_10m == 20);

    const auto duplicate = store.updateAndGet(
        makeEvent(
            100,
            "user_0",
            kBaseTsMs + 100,
            v1::LOGIN,
            v1::SUCCESS,
            ip,
            "device_duplicate",
            "login"
        ),
        kBaseTsMs + 100
    );

    assert(duplicate.ip_distinct_user_10m == 20);
}

void test_device_distinct_counts_unique_accounts() {
    FeatureStore store;

    constexpr uint64_t kBaseTsMs = 2000000;
    const std::string device_id = "shared_device";

    FeatureSnapshot snapshot;

    for (uint64_t i = 0; i < 10; ++i) {
        snapshot = store.updateAndGet(
            makeEvent(
                i,
                "account_" + std::to_string(i),
                kBaseTsMs + i,
                v1::LOGIN,
                v1::SUCCESS,
                "198.51.100." + std::to_string(i),
                device_id,
                "login"
            ),
            kBaseTsMs + i
        );
    }

    assert(snapshot.device_distinct_account_10m == 10);

    const auto duplicate = store.updateAndGet(
        makeEvent(
            100,
            "account_0",
            kBaseTsMs + 100,
            v1::LOGIN,
            v1::SUCCESS,
            "198.51.100.200",
            device_id,
            "login"
        ),
        kBaseTsMs + 100
    );

    assert(duplicate.device_distinct_account_10m == 10);
}

void test_distinct_window_expire_and_refresh() {
    FeatureStore store;

    const std::string ip = "203.0.113.9";
    const std::string device_id = "window_device";

    store.updateAndGet(
        makeEvent(1, "user_1", 0, v1::LOGIN, v1::SUCCESS, ip, device_id, "login"),
        0
    );

    const auto refreshed = store.updateAndGet(
        makeEvent(
            2,
            "user_1",
            9 * 60 * 1000,
            v1::LOGIN,
            v1::SUCCESS,
            ip,
            device_id,
            "login"
        ),
        9 * 60 * 1000
    );

    assert(refreshed.ip_distinct_user_10m == 1);
    assert(refreshed.device_distinct_account_10m == 1);

    const auto still_alive = store.updateAndGet(
        makeEvent(
            3,
            "user_2",
            11 * 60 * 1000,
            v1::LOGIN,
            v1::SUCCESS,
            ip,
            device_id,
            "login"
        ),
        11 * 60 * 1000
    );

    assert(still_alive.ip_distinct_user_10m == 2);
    assert(still_alive.device_distinct_account_10m == 2);

    const auto expired_old_member = store.updateAndGet(
        makeEvent(
            4,
            "user_3",
            20 * 60 * 1000,
            v1::LOGIN,
            v1::SUCCESS,
            ip,
            device_id,
            "login"
        ),
        20 * 60 * 1000
    );

    assert(expired_old_member.ip_distinct_user_10m == 2);
    assert(expired_old_member.device_distinct_account_10m == 2);
}

void test_attack_ip_updates_topk_and_cms() {
    FeatureStore store;

    constexpr uint64_t kBaseTsMs = 3000000;
    constexpr size_t kAttackEvents = 100;
    const std::string attack_ip = "203.0.113.10";
    const std::string attack_device = "attack_device";

    FeatureSnapshot snapshot;

    for (size_t i = 0; i < kAttackEvents; ++i) {
        snapshot = store.updateAndGet(
            makeEvent(
                i,
                "attack_user_" + std::to_string(i),
                kBaseTsMs + i,
                v1::LOGIN,
                v1::FAIL,
                attack_ip,
                attack_device,
                "login"
            ),
            kBaseTsMs + i
        );
    }

    assert(snapshot.ip_distinct_user_10m == kAttackEvents);
    assert(snapshot.device_distinct_account_10m == kAttackEvents);
    assert(snapshot.ip_topk_estimated_count >= kAttackEvents);
    assert(snapshot.ip_in_topk);
    assert(snapshot.cms_risk_behavior_count >= kAttackEvents);
}

void test_empty_ip_skips_ip_topk_and_cms() {
    FeatureStore store;

    constexpr uint64_t kNowMs = 4000000;

    const auto snapshot = store.updateAndGet(
        makeEvent(
            1,
            "user_1",
            kNowMs,
            v1::LOGIN,
            v1::FAIL,
            "",
            "device_1",
            "login"
        ),
        kNowMs
    );

    assert(snapshot.ip_distinct_user_10m == 0);
    assert(snapshot.device_distinct_account_10m == 1);
    assert(snapshot.ip_topk_estimated_count == 0);
    assert(!snapshot.ip_in_topk);
    assert(snapshot.cms_risk_behavior_count == 0);
}

void test_concurrent_different_ips() {
    FeatureStore store;

    constexpr size_t kThreads = 8;
    constexpr size_t kPerThread = 100;
    constexpr uint64_t kNowMs = 5000000;

    std::vector<std::thread> threads;

    for (size_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            const std::string ip = "198.51.100.thread_" + std::to_string(t);

            for (size_t i = 0; i < kPerThread; ++i) {
                store.updateAndGet(
                    makeEvent(
                        t * kPerThread + i,
                        "user_" + std::to_string(t) + "_" + std::to_string(i),
                        kNowMs,
                        v1::LOGIN,
                        v1::SUCCESS,
                        ip,
                        "device_" + std::to_string(t) + "_" + std::to_string(i),
                        "login"
                    ),
                    kNowMs
                );
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    for (size_t t = 0; t < kThreads; ++t) {
        const std::string ip = "198.51.100.thread_" + std::to_string(t);

        const auto snapshot = store.updateAndGet(
            makeEvent(
                100000 + t,
                "probe_user_" + std::to_string(t),
                kNowMs + 1,
                v1::LOGIN,
                v1::SUCCESS,
                ip,
                "probe_device_" + std::to_string(t),
                "login"
            ),
            kNowMs + 1
        );

        assert(snapshot.ip_distinct_user_10m == kPerThread + 1);
    }
}

void test_concurrent_hot_ip_distinct() {
    FeatureStore store;

    constexpr size_t kThreads = 8;
    constexpr size_t kPerThread = 100;
    constexpr uint64_t kNowMs = 6000000;
    const std::string hot_ip = "203.0.113.hot";

    std::vector<std::thread> threads;

    for (size_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (size_t i = 0; i < kPerThread; ++i) {
                store.updateAndGet(
                    makeEvent(
                        t * kPerThread + i,
                        "hot_user_" + std::to_string(t) + "_" + std::to_string(i),
                        kNowMs,
                        v1::LOGIN,
                        v1::FAIL,
                        hot_ip,
                        "hot_device",
                        "login"
                    ),
                    kNowMs
                );
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    const auto snapshot = store.updateAndGet(
        makeEvent(
            999999,
            "hot_probe_user",
            kNowMs + 1,
            v1::LOGIN,
            v1::FAIL,
            hot_ip,
            "hot_device",
            "login"
        ),
        kNowMs + 1
    );

    assert(snapshot.ip_distinct_user_10m == kThreads * kPerThread + 1);
    assert(snapshot.device_distinct_account_10m == kThreads * kPerThread + 1);
    assert(snapshot.ip_topk_estimated_count >= kThreads * kPerThread + 1);
    assert(snapshot.ip_in_topk);
    assert(snapshot.cms_risk_behavior_count >= kThreads * kPerThread + 1);
}

int main() {
    test_ip_distinct_counts_unique_users();
    test_device_distinct_counts_unique_accounts();
    test_distinct_window_expire_and_refresh();
    test_attack_ip_updates_topk_and_cms();
    test_empty_ip_skips_ip_topk_and_cms();
    test_concurrent_different_ips();
    test_concurrent_hot_ip_distinct();

    std::cout << "test_feature_store_ip_device passed" << std::endl;
    return 0;
}
