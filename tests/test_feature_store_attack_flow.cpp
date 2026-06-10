#include "aegisflow/feature/feature_store.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

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

FeatureSnapshot feedNormalTraffic(FeatureStore& store, uint64_t base_ts_ms) {
    constexpr size_t kUserNum = 10000;
    constexpr size_t kIpNum = 1000;
    constexpr size_t kDeviceNum = 5000;

    FeatureSnapshot snapshot;

    for (size_t i = 0; i < kUserNum; ++i) {
        const uint64_t event_id = i + 1;
        const uint64_t ts_ms = base_ts_ms + i;

        snapshot = store.updateAndGet(
            makeEvent(
                event_id,
                "normal_user_" + std::to_string(i),
                ts_ms,
                v1::LOGIN,
                v1::SUCCESS,
                "198.51.100." + std::to_string(i % kIpNum),
                "normal_device_" + std::to_string(i % kDeviceNum),
                "login"
            ),
            ts_ms
        );
    }

    return snapshot;
}

FeatureSnapshot feedAttackTraffic(FeatureStore& store, uint64_t base_ts_ms) {
    constexpr size_t kAttackUserNum = 1000;
    const std::string attack_ip = "203.0.113.10";
    const std::string attack_device = "attack_device";

    FeatureSnapshot snapshot;

    for (size_t i = 0; i < kAttackUserNum; ++i) {
        const uint64_t event_id = 100000 + i;
        const uint64_t ts_ms = base_ts_ms + i;

        snapshot = store.updateAndGet(
            makeEvent(
                event_id,
                "attack_user_" + std::to_string(i),
                ts_ms,
                v1::LOGIN,
                v1::FAIL,
                attack_ip,
                attack_device,
                "login"
            ),
            ts_ms
        );
    }

    return snapshot;
}

void test_attack_ip_enters_topk_under_acceptance_traffic() {
    FeatureStore store;

    constexpr uint64_t kBaseTsMs = 1000000;
    constexpr size_t kNormalEvents = 10000;
    constexpr size_t kNormalIpNum = 1000;
    constexpr size_t kAttackUserNum = 1000;

    feedNormalTraffic(store, kBaseTsMs);

    const auto attack_snapshot = feedAttackTraffic(
        store,
        kBaseTsMs + kNormalEvents
    );

    assert(attack_snapshot.ip_distinct_user_10m == kAttackUserNum);
    assert(attack_snapshot.device_distinct_account_10m == kAttackUserNum);
    assert(attack_snapshot.ip_topk_estimated_count >= kAttackUserNum);
    assert(attack_snapshot.ip_in_topk);
    assert(attack_snapshot.cms_risk_behavior_count >= kAttackUserNum);

    const auto normal_probe = store.updateAndGet(
        makeEvent(
            200000,
            "normal_probe_user",
            kBaseTsMs + kNormalEvents + kAttackUserNum + 1,
            v1::LOGIN,
            v1::SUCCESS,
            "198.51.100.1",
            "normal_probe_device",
            "login"
        ),
        kBaseTsMs + kNormalEvents + kAttackUserNum + 1
    );

    assert(normal_probe.ip_topk_estimated_count <= kNormalEvents / kNormalIpNum + 1);
}

int main() {
    test_attack_ip_enters_topk_under_acceptance_traffic();

    std::cout << "test_feature_store_attack_flow passed" << std::endl;
    return 0;
}