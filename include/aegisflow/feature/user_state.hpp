#pragma once

#include <cstdint>
#include "aegisflow/feature/sliding_counter.hpp"

namespace aegisflow::feature {

struct LoginFeatureSnapshot {
    std::uint32_t user_login_fail_5m = 0;
    std::uint32_t ip_distinct_failed_user_10m = 0;
    std::uint32_t device_distinct_account_10m = 0;
    std::uint32_t ip_login_failures_10m = 0;
};

struct LoginUserState {
    SlidingCounter<60> login_fail_5m{5000};

    std::uint64_t last_seen_ms = 0;
};

}  // 命名空间 aegisflow::feature
