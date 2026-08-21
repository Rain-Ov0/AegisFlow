#include "aegisflow/risk/login_policy_chain.hpp"

#include "aegisflow/feature/feature_store.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>

namespace aegisflow::risk {
namespace {

constexpr std::int32_t kReviewScore = 30;
constexpr std::int32_t kRejectScore = 100;

void appendHit(
    std::vector<aegisflow::domain::PolicyHit>& hits,
    const std::string_view reason,
    const aegisflow::domain::LoginDecisionAction action,
    const std::int32_t score
) {
    hits.push_back({std::string(reason), action, score});
}

void evaluateBlacklist(
    const LoginPolicyContext& context,
    std::vector<aegisflow::domain::PolicyHit>& hits
) {
    if (context.blacklist.user_hit) {
        appendHit(
            hits,
            "blacklisted_user",
            aegisflow::domain::LoginDecisionAction::reject,
            kRejectScore
        );
    }
    if (context.blacklist.ip_hit) {
        appendHit(
            hits,
            "blacklisted_ip",
            aegisflow::domain::LoginDecisionAction::reject,
            kRejectScore
        );
    }
    if (context.blacklist.device_hit) {
        appendHit(
            hits,
            "blacklisted_device",
            aegisflow::domain::LoginDecisionAction::reject,
            kRejectScore
        );
    }
}

void evaluateCredentialStuffing(
    const LoginPolicyContext& context,
    const LoginPolicyConfig& config,
    std::vector<aegisflow::domain::PolicyHit>& hits
) {
    if (context.features.ip_distinct_failed_user_10m >=
            config.ip_distinct_reject_threshold &&
        context.features.ip_login_failures_10m >=
            config.ip_failure_reject_threshold) {
        appendHit(
            hits,
            "credential_stuffing_attack",
            aegisflow::domain::LoginDecisionAction::reject,
            kRejectScore
        );
    }
}

void evaluateUserFailures(
    const LoginPolicyContext& context,
    const LoginPolicyConfig& config,
    std::vector<aegisflow::domain::PolicyHit>& hits
) {
    if (context.features.user_login_fail_5m >=
        config.user_failure_review_threshold) {
        appendHit(
            hits,
            "too_many_failed_login",
            aegisflow::domain::LoginDecisionAction::review,
            kReviewScore
        );
    }
}

void evaluateIpSpray(
    const LoginPolicyContext& context,
    const LoginPolicyConfig& config,
    std::vector<aegisflow::domain::PolicyHit>& hits
) {
    if (context.features.ip_distinct_failed_user_10m >=
        config.ip_spray_review_threshold) {
        appendHit(
            hits,
            "ip_many_users_failed_login",
            aegisflow::domain::LoginDecisionAction::review,
            kReviewScore
        );
    }
}

void evaluateDeviceSharing(
    const LoginPolicyContext& context,
    const LoginPolicyConfig& config,
    std::vector<aegisflow::domain::PolicyHit>& hits
) {
    if (context.features.device_distinct_account_10m >=
        config.device_sharing_review_threshold) {
        appendHit(
            hits,
            "device_many_accounts",
            aegisflow::domain::LoginDecisionAction::review,
            kReviewScore
        );
    }
}

int severity(const aegisflow::domain::LoginDecisionAction action) noexcept {
    switch (action) {
        case aegisflow::domain::LoginDecisionAction::pass:
            return 0;
        case aegisflow::domain::LoginDecisionAction::review:
            return 1;
        case aegisflow::domain::LoginDecisionAction::reject:
            return 2;
    }
    return 0;
}

}  // 命名空间

void validateLoginPolicyConfig(const LoginPolicyConfig& config) {
    if (config.user_failure_review_threshold == 0 ||
        config.ip_spray_review_threshold == 0 ||
        config.device_sharing_review_threshold == 0 ||
        config.ip_distinct_reject_threshold == 0 ||
        config.ip_failure_reject_threshold == 0) {
        throw std::invalid_argument("登录策略阈值必须大于 0");
    }
    if (config.ip_distinct_reject_threshold <
        config.ip_spray_review_threshold) {
        throw std::invalid_argument(
            "IP 拒绝 distinct 阈值不得小于复核阈值"
        );
    }

    constexpr auto capacity =
        aegisflow::feature::LoginFeatureStore::kDistinctMaxMembers;
    if (config.ip_spray_review_threshold > capacity ||
        config.device_sharing_review_threshold > capacity ||
        config.ip_distinct_reject_threshold > capacity) {
        throw std::invalid_argument("登录 distinct 策略阈值超过成员上限");
    }
}

LoginPolicyChain::LoginPolicyChain(const LoginPolicyConfig& config)
    : config_(config) {
    validateLoginPolicyConfig(config_);
}

aegisflow::domain::LoginDecision LoginPolicyChain::evaluate(
    const aegisflow::domain::LoginAttempt& attempt,
    const aegisflow::feature::LoginFeatureSnapshot& features,
    const LoginBlacklistMatches& blacklist
) const {
    aegisflow::domain::LoginDecision decision;
    decision.attempt_id = attempt.attemptId();
    decision.user_id = attempt.userId();

    const LoginPolicyContext context{features, blacklist};
    // 顺序固定为黑名单、撞库、用户失败、IP 扫号、设备共享，reason 的顺序因而可稳定复现。
    evaluateBlacklist(context, decision.policy_hits);
    evaluateCredentialStuffing(context, config_, decision.policy_hits);
    evaluateUserFailures(context, config_, decision.policy_hits);
    evaluateIpSpray(context, config_, decision.policy_hits);
    evaluateDeviceSharing(context, config_, decision.policy_hits);

    // 不短路返回：动作取最严重值，分数取最大值，命中详情全部保留。
    for (const auto& hit : decision.policy_hits) {
        if (severity(hit.action) > severity(decision.action)) {
            decision.action = hit.action;
        }
        decision.risk_score = std::max(decision.risk_score, hit.risk_score);
    }
    return decision;
}

}  // 命名空间 aegisflow::risk
