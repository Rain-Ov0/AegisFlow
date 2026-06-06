#include "aegisflow/feature/user_state.hpp"

#include <cassert>
#include <iostream>

using aegisflow::feature::FeatureSnapshot;
using aegisflow::feature::RecentAction;
using aegisflow::feature::UserState;

void test_default_user_state() {
    UserState state;

    assert(state.last_seen_ms == 0);
    assert(state.login_1m.sum(0) == 0);
    assert(state.login_5m.sum(0) == 0);
    assert(state.login_1h.sum(0) == 0);
    assert(state.login_fail_5m.sum(0) == 0);
    assert(state.recent_actions.empty());
}

void test_update_user_state_and_snapshot() {
    UserState state;

    const uint64_t now_ms = 1000;
    state.last_seen_ms = now_ms;

    state.login_1m.add(now_ms, now_ms);
    state.login_5m.add(now_ms, now_ms);
    state.login_1h.add(now_ms, now_ms);
    state.login_fail_5m.add(now_ms, now_ms);

    state.recent_actions.push({
        aegisflow::v1::LOGIN,
        aegisflow::v1::FAIL,
        now_ms,
    });

    FeatureSnapshot snapshot;
    snapshot.user_id = "user_001";
    snapshot.user_login_1m = state.login_1m.sum(now_ms);
    snapshot.user_login_5m = state.login_5m.sum(now_ms);
    snapshot.user_login_1h = state.login_1h.sum(now_ms);
    snapshot.user_login_fail_5m = state.login_fail_5m.sum(now_ms);
    snapshot.recent_actions = state.recent_actions.list();

    assert(snapshot.user_id == "user_001");
    assert(snapshot.user_login_1m == 1);
    assert(snapshot.user_login_5m == 1);
    assert(snapshot.user_login_1h == 1);
    assert(snapshot.user_login_fail_5m == 1);
    assert(snapshot.recent_actions.size() == 1);
    assert(snapshot.recent_actions[0].type == aegisflow::v1::LOGIN);
    assert(snapshot.recent_actions[0].result == aegisflow::v1::FAIL);
}

int main() {
    test_default_user_state();
    test_update_user_state_and_snapshot();

    std::cout << "test_user_state passed" << std::endl;
    return 0;
}