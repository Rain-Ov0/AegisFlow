#pragma once

#include "aegisflow/domain/login.hpp"
#include "aegisflow/feature/user_state.hpp"
#include "aegisflow/risk/blacklist_snapshot.hpp"

#include <cstdint>
namespace aegisflow::risk {

struct LoginPolicyConfig {
    std::uint32_t user_failure_review_threshold = 5;
    std::uint32_t ip_spray_review_threshold = 20;
    std::uint32_t device_sharing_review_threshold = 10;
    std::uint32_t ip_distinct_reject_threshold = 50;
    std::uint32_t ip_failure_reject_threshold = 500;

    bool operator==(const LoginPolicyConfig& other) const {
        return user_failure_review_threshold ==
                   other.user_failure_review_threshold &&
               ip_spray_review_threshold ==
                   other.ip_spray_review_threshold &&
               device_sharing_review_threshold ==
                   other.device_sharing_review_threshold &&
               ip_distinct_reject_threshold ==
                   other.ip_distinct_reject_threshold &&
               ip_failure_reject_threshold ==
                   other.ip_failure_reject_threshold;
    }
};

void validateLoginPolicyConfig(const LoginPolicyConfig& config);

struct LoginPolicyContext {
    const aegisflow::feature::LoginFeatureSnapshot& features;
    const LoginBlacklistMatches& blacklist;
};

}  // 命名空间 aegisflow::risk
