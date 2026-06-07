#include "aegisflow/feature/feature_store.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace v1 = aegisflow::v1;
using aegisflow::feature::FeatureStore;

v1::Event makeEvent(
    uint64_t event_id,
    const std::string& user_id,
    uint64_t timestamp_ms,
    v1::EventType type = v1::LOGIN,
    v1::EventResult result = v1::SUCCESS
) {
    v1::Event event;
    event.set_event_id(event_id);
    event.set_user_id(user_id);
    event.set_timestamp_ms(timestamp_ms);
    event.set_type(type);
    event.set_result(result);
    return event;
}

auto snapshotAt(FeatureStore& store, const std::string& user_id, uint64_t now_ms) {
    return store.updateAndGet(
        makeEvent(999999, user_id, now_ms, v1::PAY, v1::SUCCESS),
        now_ms
    );
}

void test_login_counters() {
    FeatureStore store;

    for (uint64_t i = 0; i < 10; ++ i) {
        store.updateAndGet(makeEvent(i, "u1", 1000 + i), 1000 + i);
    }

    const auto s = snapshotAt(store, "u1", 1010);
    assert(s.user_login_1m == 10);
    assert(s.user_login_5m == 10);
    assert(s.user_login_1h == 10);
    assert(s.user_login_fail_5m == 0);
}

void test_window_boundaries() {
    FeatureStore one_min;
    one_min.updateAndGet(makeEvent(1, "u1", 0), 0);
    assert(snapshotAt(one_min, "u1", 59000).user_login_1m == 1);
    assert(snapshotAt(one_min, "u1", 60000).user_login_1m == 0);

    FeatureStore five_min;
    five_min.updateAndGet(makeEvent(1, "u1", 0), 0);
    assert(snapshotAt(five_min, "u1", 299000).user_login_5m == 1);
    assert(snapshotAt(five_min, "u1", 300000).user_login_5m == 0);

    FeatureStore one_hour;
    one_hour.updateAndGet(makeEvent(1, "u1", 0), 0);
    assert(snapshotAt(one_hour, "u1", 3599000).user_login_1h == 1);
    assert(snapshotAt(one_hour, "u1", 3600000).user_login_1h == 0);
}

void test_event_type_and_result() {
    FeatureStore store;

    auto pay = store.updateAndGet(makeEvent(1, "u1", 1000, v1::PAY, v1::SUCCESS), 1000);
    assert(pay.user_login_1m == 0);

    auto fail = store.updateAndGet(makeEvent(2, "u1", 1001, v1::LOGIN, v1::FAIL), 1001);
    assert(fail.user_login_1m == 1);    
    assert(fail.user_login_fail_5m == 1);

    auto success = store.updateAndGet(makeEvent(3, "u1", 1002, v1::LOGIN, v1::SUCCESS), 1002);
    assert(success.user_login_1m == 2);
    assert(success.user_login_fail_5m == 1);
}

void test_invalid_event_time_and_empty_user() {
    FeatureStore store;

    auto empty = store.updateAndGet(makeEvent(1, "", 1000), 1000);
    assert(empty.user_id.empty());
    assert(empty.user_login_1m == 0);
    assert(empty.user_login_fail_5m == 0);

    store.updateAndGet(makeEvent(2, "u1", 3600000), 3600000);

    auto old_event = store.updateAndGet(makeEvent(3, "u1", 0), 3600000);
    assert(old_event.user_login_1h == 1);
    assert(old_event.recent_actions.size() == 1);
}

void test_recent_actions_keep_latest_20() {
    FeatureStore store;

    aegisflow::feature::FeatureSnapshot s;
    for (uint64_t i = 0; i < 25; ++i) {
        s = store.updateAndGet(makeEvent(i, "u1", 1000 + i), 1000 + i);
    }

    assert(s.recent_actions.size() == 20);
    for (uint64_t i = 0; i < 20; ++i) {
        assert(s.recent_actions[i].timestamp_ms == 1005 + i);
    }
}

void test_concurrent_same_user() {
    FeatureStore store;
    constexpr size_t kThreads = 8;
    constexpr size_t kPerThread = 200;
    constexpr uint64_t kNow = 1000000;

    std::vector<std::thread> threads;
    for (size_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (size_t i = 0; i < kPerThread; ++i) {
                store.updateAndGet(makeEvent(t * kPerThread + i, "same_user", kNow), kNow);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    const auto s = snapshotAt(store, "same_user", kNow);
    assert(s.user_login_1m == kThreads * kPerThread);
}

void test_concurrent_different_users() {
    FeatureStore store;
    constexpr size_t kUsers = 8;
    constexpr size_t kPerUser = 100;
    constexpr uint64_t kNow = 2000000;

    std::vector<std::thread> threads;
    for (size_t u = 0; u < kUsers; ++u) {
        threads.emplace_back([&, u]() {
            const std::string user_id = "user_" + std::to_string(u);
            for (size_t i = 0; i < kPerUser; ++i) {
                store.updateAndGet(makeEvent(u * kPerUser + i, user_id, kNow), kNow);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    for (size_t u = 0; u < kUsers; ++u) {
        const auto s = snapshotAt(store, "user_" + std::to_string(u), kNow);
        assert(s.user_login_1m == kPerUser);
    }
}

int main() {
    test_login_counters();
    test_window_boundaries();
    test_event_type_and_result();
    test_invalid_event_time_and_empty_user();
    test_recent_actions_keep_latest_20();
    test_concurrent_same_user();
    test_concurrent_different_users();

    std::cout << "test_feature_store passed" << std::endl;
    return 0;
}