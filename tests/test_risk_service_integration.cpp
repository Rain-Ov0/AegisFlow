#include "aegisflow/app/risk_service.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

namespace v1 = aegisflow::v1;

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

v1::ReportEventRequest makeLoginFailRequest(
    uint64_t event_id,
    const std::string& user_id,
    uint64_t timestamp_ms
) {
    v1::ReportEventRequest request;
    auto* event = request.mutable_event();

    event->set_event_id(event_id);
    event->set_timestamp_ms(timestamp_ms);
    event->set_user_id(user_id);
    event->set_ip("203.0.113.66");
    event->set_device_id("device_login_fail_review");
    event->set_scene("login");
    event->set_type(v1::LOGIN);
    event->set_result(v1::FAIL);

    return request;
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

int main() {
    test_five_failed_login_returns_review();

    std::cout << "test_risk_service_integration passed" << std::endl;
    return 0;
}