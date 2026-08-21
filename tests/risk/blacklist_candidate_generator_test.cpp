#include "aegisflow/app/login_request_validator.hpp"
#include "aegisflow/risk/blacklist_candidate_generator.hpp"

#include "tests/support/test_harness.hpp"

#include "login.pb.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string_view>

namespace {

using aegisflow::test::require;

aegisflow::domain::LoginAttempt validAttempt(const std::uint64_t now_ms) {
    aegisflow::login::LoginRequest request;
    request.set_attempt_id(91);
    request.set_timestamp_ms(now_ms);
    request.set_user_id("candidate-user");
    request.set_ip("2001:0db8:0:0:0:0:0:91");
    request.set_device_id("candidate-device");
    request.set_result(aegisflow::login::FAIL);
    auto attempt = aegisflow::app::LoginRequestValidator::validate(
        request,
        now_ms
    );
    require(attempt.has_value(), "候选测试请求必须通过集中校验");
    return std::move(*attempt);
}

aegisflow::domain::LoginDecision credentialStuffingReject() {
    aegisflow::domain::LoginDecision decision;
    decision.action = aegisflow::domain::LoginDecisionAction::reject;
    decision.policy_hits.push_back({
        "credential_stuffing_attack",
        aegisflow::domain::LoginDecisionAction::reject,
        100,
    });
    return decision;
}

void credentialStuffingCreatesCanonicalTemporaryIp() {
    constexpr std::uint64_t now_ms = 1'700'000'000'123ULL;
    const auto attempt = validAttempt(now_ms);
    const auto decision = credentialStuffingReject();
    const aegisflow::risk::BlacklistCandidateGenerator generator;

    const auto first = generator.generate(attempt, decision, {}, now_ms);
    const auto second = generator.generate(attempt, decision, {}, now_ms);
    require(first.size() == 1 && second == first, "固定 now_ms 的结果必须可重复");

    const auto& candidate = first.front();
    require(
        candidate.operation() ==
                aegisflow::risk::BlacklistMutationOperation::Upsert &&
            candidate.entityType() == aegisflow::risk::EntityType::Ip &&
            candidate.id() == "2001:db8::91" &&
            candidate.reason() == "credential_stuffing_attack" &&
            candidate.expireAtMs() ==
                now_ms +
                    aegisflow::risk::BlacklistCandidateGenerator::
                        kTemporaryIpTtlMs,
        "撞库 REJECT 必须只产生一条 30 分钟的规范 IP UPSERT"
    );
}

void everyReviewReasonCreatesNothing() {
    constexpr std::uint64_t now_ms = 1'700'000'000'123ULL;
    const auto attempt = validAttempt(now_ms);
    const aegisflow::risk::BlacklistCandidateGenerator generator;
    constexpr std::array<std::string_view, 3> reasons = {
        "too_many_failed_login",
        "ip_many_users_failed_login",
        "device_many_accounts",
    };

    for (const auto reason : reasons) {
        aegisflow::domain::LoginDecision decision;
        decision.action = aegisflow::domain::LoginDecisionAction::review;
        decision.policy_hits.push_back({
            std::string(reason),
            aegisflow::domain::LoginDecisionAction::review,
            30,
        });
        require(
            generator.generate(attempt, decision, {}, now_ms).empty(),
            "REVIEW 不得自动升级为黑名单"
        );
    }
}

void anyExistingBlacklistMatchSuppressesCandidate() {
    constexpr std::uint64_t now_ms = 1'700'000'000'123ULL;
    const auto attempt = validAttempt(now_ms);
    const auto decision = credentialStuffingReject();
    const aegisflow::risk::BlacklistCandidateGenerator generator;
    std::array<aegisflow::risk::LoginBlacklistMatches, 3> matches{};
    matches[0].user_hit = true;
    matches[1].ip_hit = true;
    matches[2].device_hit = true;

    for (const auto& item : matches) {
        require(
            generator.generate(attempt, decision, item, now_ms).empty(),
            "任一现有黑名单命中时不得再产生候选"
        );
    }
}

void exactRejectHitAndSafeExpiryAreRequired() {
    constexpr std::uint64_t now_ms = 1'700'000'000'123ULL;
    const auto attempt = validAttempt(now_ms);
    const aegisflow::risk::BlacklistCandidateGenerator generator;

    aegisflow::domain::LoginDecision other_reject;
    other_reject.action = aegisflow::domain::LoginDecisionAction::reject;
    other_reject.policy_hits.push_back({
        "other_reject",
        aegisflow::domain::LoginDecisionAction::reject,
        100,
    });
    require(
        generator.generate(attempt, other_reject, {}, now_ms).empty(),
        "其他 REJECT 不得生成自动封禁"
    );

    auto wrong_hit_action = credentialStuffingReject();
    wrong_hit_action.policy_hits.front().action =
        aegisflow::domain::LoginDecisionAction::review;
    require(
        generator.generate(attempt, wrong_hit_action, {}, now_ms).empty(),
        "credential reason 必须本身是 REJECT 命中"
    );

    const auto decision = credentialStuffingReject();
    const auto overflow_now =
        aegisflow::risk::BlacklistMutation::kMaxExpireAtMs -
        aegisflow::risk::BlacklistCandidateGenerator::kTemporaryIpTtlMs + 1;
    require(
        generator.generate(attempt, decision, {}, overflow_now).empty(),
        "30 分钟过期时间溢出时必须拒绝候选"
    );
}

}  // namespace

int main() {
    return aegisflow::test::runModule(
        "blacklist_candidate_generator",
        {
            {"credential stuffing IP 候选", credentialStuffingCreatesCanonicalTemporaryIp},
            {"REVIEW 不封禁", everyReviewReasonCreatesNothing},
            {"已有黑名单不生成", anyExistingBlacklistMatchSuppressesCandidate},
            {"精确命中与过期边界", exactRejectHitAndSafeExpiryAreRequired},
        }
    );
}
