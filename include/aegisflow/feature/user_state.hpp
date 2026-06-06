#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "aegisflow/feature/recent_action_window.hpp"
#include "aegisflow/feature/sliding_counter.hpp"

namespace aegisflow::feature {

struct FeatureSnapshot {
    std::string user_id;

    uint32_t user_login_1m = 0;
    uint32_t user_login_5m = 0;
    uint32_t user_login_1h = 0;
    uint32_t user_login_fail_5m = 0;

    std::vector<RecentAction> recent_actions;
};

struct UserState {
    SlidingCounter<60> login_1m{1000};
    SlidingCounter<60> login_5m{5000};
    SlidingCounter<60> login_1h{60000};
    SlidingCounter<60> login_fail_5m{5000};

    RecentActionWindow<20> recent_actions;

    uint64_t last_seen_ms = 0;
};

} // namespace aegisflow::feature
