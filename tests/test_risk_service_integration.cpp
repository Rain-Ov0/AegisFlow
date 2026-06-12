#include "aegisflow/app/risk_service.hpp"
#include "aegisflow/risk/blacklist_manager.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

namespace v1 = aegisflow::v1;

using aegisflow::risk::BlacklistEntry;
using aegisflow::risk::BlacklistManager;
using aegisflow::risk::BlacklistManagerOptions;
using aegisflow::risk::EntityType;

#ifndef AEGISFLOW_TEST_RULE_FILE
#define AEGISFLOW_TEST_RULE_FILE "config/rules.dsl"
#endif

uint64_t nowMillis() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()
        ).count()
    );
}

BlacklistManagerOptions testBlacklistOptions() {
    BlacklistManagerOptions options;
    options.bloom_bits = 1 << 16;
    options.bloom_hashes = 5;
    options.cache_capacity = 64;
    options.positive_ttl_ms = 60ULL * 1000ULL;
    options.negative_ttl_ms = 1000ULL;
    return options;
}

v1::ReportEventRequest makeLoginRequest(
    uint64_t event_id,
    const std::string& user_id,
    uint64_t timestamp_ms,
    v1::EventResult result,
    const std::string& ip = "203.0.113.66",
    const std::string& device_id = "device_login_test"
) {
    v1::ReportEventRequest request;
    auto* event = request.mutable_event();

    event->set_event_id(event_id);
    event->set_timestamp_ms(timestamp_ms);
    event->set_user_id(user_id);
    event->set_ip(ip);
    event->set_device_id(device_id);
    event->set_scene("login");
    event->set_type(v1::LOGIN);
    event->set_result(result);

    return request;
}

v1::ReportEventRequest makeLoginFailRequest(
    uint64_t event_id,
    const std::string& user_id,
    uint64_t timestamp_ms
) {
    return makeLoginRequest(event_id, user_id, timestamp_ms, v1::FAIL);
}

void assertNoReason(const v1::Decision& decision) {
    assert(decision.reasons_size() == 0);
}

void assertSingleReason(
    const v1::Decision& decision,
    const std::string& expected_code
) {
    assert(decision.reasons_size() == 1);
    assert(decision.reasons(0).code() == expected_code);
    assert(decision.reasons(0).message() == expected_code);
}

void test_five_failed_login_returns_review() {
    aegisflow::app::RiskService service(1, AEGISFLOW_TEST_RULE_FILE);

    const std::string user_id = "week4_day6_user";

    for (uint64_t i = 1; i <= 4; ++i) {
        const auto response = service.handleEvent(
            makeLoginFailRequest(i, user_id, nowMillis())
        );

        const auto& decision = response.decision();
        assert(decision.event_id() == i);
        assert(decision.user_id() == user_id);
        assert(decision.action() == v1::PASS);
        assert(decision.risk_score() == 0);
        assertNoReason(decision);
        assert(decision.features().user_login_fail_5m() == i);
    }

    const auto response = service.handleEvent(
        makeLoginFailRequest(5, user_id, nowMillis())
    );

    const auto& decision = response.decision();
    assert(decision.event_id() == 5);
    assert(decision.user_id() == user_id);
    assert(decision.action() == v1::REVIEW);
    assert(decision.risk_score() == 30);
    assertSingleReason(decision, "too_many_failed_login");
    assert(decision.features().user_login_fail_5m() == 5);
}

void test_blacklisted_user_returns_reject() {
    BlacklistManager blacklist_manager(nullptr, testBlacklistOptions());
    blacklist_manager.loadEntries({
        {EntityType::User, "u_black_001", "blacklisted_user", 0},
    });

    aegisflow::app::RiskService service(
        1,
        AEGISFLOW_TEST_RULE_FILE,
        &blacklist_manager
    );

    const auto response = service.handleEvent(
        makeLoginRequest(
            100,
            "u_black_001",
            nowMillis(),
            v1::SUCCESS,
            "198.51.100.10",
            "device_blacklist_test"
        )
    );

    const auto& decision = response.decision();
    assert(decision.event_id() == 100);
    assert(decision.user_id() == "u_black_001");
    assert(decision.action() == v1::REJECT);
    assert(decision.risk_score() == 100);
    assertSingleReason(decision, "blacklisted_user");

    assert(decision.features().user_black_hit());
    assert(!decision.features().ip_black_hit());
    assert(!decision.features().device_black_hit());
    assert(decision.features().blacklist_reason() == "blacklisted_user");
}

int main() {
    test_five_failed_login_returns_review();
    test_blacklisted_user_returns_reject();

    std::cout << "test_risk_service_integration passed" << std::endl;
    return 0;
}