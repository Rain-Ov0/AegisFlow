#include "aegisflow/app/login_request_validator.hpp"
#include "aegisflow/risk/login_policy_chain.hpp"

#include "tests/support/test_harness.hpp"

#include "login.pb.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace {

using aegisflow::test::require;

aegisflow::domain::LoginAttempt validAttempt(const std::uint64_t now_ms) {
    aegisflow::login::LoginRequest request;
    request.set_attempt_id(7);
    request.set_timestamp_ms(now_ms);
    request.set_user_id("user-7");
    request.set_ip("203.0.113.7");
    request.set_device_id("device-7");
    request.set_result(aegisflow::login::FAIL);
    auto validation = aegisflow::app::LoginRequestValidator::validate(
        request,
        now_ms
    );
    require(validation.has_value(), "策略测试请求必须通过校验");
    return std::move(*validation);
}

void belowThresholdsPass() {
    constexpr std::uint64_t now_ms = 10'000'000;
    const aegisflow::risk::LoginPolicyConfig config;
    const aegisflow::risk::LoginPolicyChain chain(config);

    aegisflow::feature::LoginFeatureSnapshot features;
    features.user_login_fail_5m = config.user_failure_review_threshold - 1;
    features.ip_distinct_failed_user_10m =
        config.ip_spray_review_threshold - 1;
    features.device_distinct_account_10m =
        config.device_sharing_review_threshold - 1;
    features.ip_login_failures_10m = config.ip_failure_reject_threshold - 1;

    const auto decision = chain.evaluate(validAttempt(now_ms), features, {});
    require(
        decision.action == aegisflow::domain::LoginDecisionAction::pass &&
            decision.risk_score == 0 && decision.policy_hits.empty(),
        "未到阈值时必须 PASS"
    );
}

void reviewRulesRemainIndependent() {
    constexpr std::uint64_t now_ms = 10'000'000;
    const aegisflow::risk::LoginPolicyConfig config;
    const aegisflow::risk::LoginPolicyChain chain(config);

    struct ReviewCase {
        std::uint32_t aegisflow::feature::LoginFeatureSnapshot::*field;
        std::uint32_t threshold;
        std::string_view reason;
    };
    const std::array<ReviewCase, 3> cases = {{
        {&aegisflow::feature::LoginFeatureSnapshot::user_login_fail_5m,
         config.user_failure_review_threshold,
         "too_many_failed_login"},
        {&aegisflow::feature::LoginFeatureSnapshot::ip_distinct_failed_user_10m,
         config.ip_spray_review_threshold,
         "ip_many_users_failed_login"},
        {&aegisflow::feature::LoginFeatureSnapshot::device_distinct_account_10m,
         config.device_sharing_review_threshold,
         "device_many_accounts"},
    }};

    for (const auto& item : cases) {
        aegisflow::feature::LoginFeatureSnapshot features;
        features.*(item.field) = item.threshold;
        const auto decision = chain.evaluate(validAttempt(now_ms), features, {});
        require(
            decision.action == aegisflow::domain::LoginDecisionAction::review &&
                decision.risk_score == 30 && decision.policy_hits.size() == 1 &&
                decision.policy_hits.front().reason_code == item.reason,
            "REVIEW 规则的阈值、动作或 reason 发生变化"
        );
    }
}

void credentialStuffingRequiresBothSignals() {
    constexpr std::uint64_t now_ms = 10'000'000;
    const aegisflow::risk::LoginPolicyConfig config;
    const aegisflow::risk::LoginPolicyChain chain(config);
    aegisflow::feature::LoginFeatureSnapshot features;
    features.ip_distinct_failed_user_10m = config.ip_distinct_reject_threshold;
    features.ip_login_failures_10m = config.ip_failure_reject_threshold - 1;

    auto decision = chain.evaluate(validAttempt(now_ms), features, {});
    require(
        decision.action != aegisflow::domain::LoginDecisionAction::reject,
        "只有 distinct 信号时不得触发撞库 REJECT"
    );

    features.ip_distinct_failed_user_10m = config.ip_distinct_reject_threshold - 1;
    features.ip_login_failures_10m = config.ip_failure_reject_threshold;
    decision = chain.evaluate(validAttempt(now_ms), features, {});
    require(
        decision.action != aegisflow::domain::LoginDecisionAction::reject,
        "只有失败次数信号时不得触发撞库 REJECT"
    );

    features.ip_distinct_failed_user_10m = config.ip_distinct_reject_threshold;
    decision = chain.evaluate(validAttempt(now_ms), features, {});
    require(
        decision.action == aegisflow::domain::LoginDecisionAction::reject &&
            decision.policy_hits.front().reason_code ==
                "credential_stuffing_attack",
        "两个撞库信号均达阈值时必须 REJECT"
    );
}

void allHitsRemainOrderedAndMostSevereWins() {
    constexpr std::uint64_t now_ms = 10'000'000;
    const aegisflow::risk::LoginPolicyConfig config;
    const aegisflow::risk::LoginPolicyChain chain(config);

    aegisflow::feature::LoginFeatureSnapshot features;
    features.user_login_fail_5m = config.user_failure_review_threshold;
    features.ip_distinct_failed_user_10m = config.ip_distinct_reject_threshold;
    features.device_distinct_account_10m =
        config.device_sharing_review_threshold;
    features.ip_login_failures_10m = config.ip_failure_reject_threshold;

    aegisflow::risk::LoginBlacklistMatches matches;
    matches.user_hit = true;
    matches.ip_hit = true;
    matches.device_hit = true;

    const auto decision = chain.evaluate(
        validAttempt(now_ms),
        features,
        matches
    );
    constexpr std::array<std::string_view, 7> reasons = {
        "blacklisted_user",
        "blacklisted_ip",
        "blacklisted_device",
        "credential_stuffing_attack",
        "too_many_failed_login",
        "ip_many_users_failed_login",
        "device_many_accounts",
    };
    require(decision.policy_hits.size() == reasons.size(), "必须保留全部策略命中");
    for (std::size_t index = 0; index < reasons.size(); ++index) {
        require(
            decision.policy_hits[index].reason_code == reasons[index],
            "策略 reason code 或执行顺序发生变化"
        );
    }
    require(
        decision.action == aegisflow::domain::LoginDecisionAction::reject &&
            decision.risk_score == 100 && decision.attempt_id == 7 &&
            decision.user_id == "user-7",
        "最终动作、分数或请求关联字段不一致"
    );
}

}  // namespace

int main() {
    return aegisflow::test::runModule(
        "login_policy",
        {
            {"阈值下 PASS", belowThresholdsPass},
            {"REVIEW 规则独立", reviewRulesRemainIndependent},
            {"撞库双信号", credentialStuffingRequiresBothSignals},
            {"全量命中顺序", allHitsRemainOrderedAndMostSevereWins},
        }
    );
}
