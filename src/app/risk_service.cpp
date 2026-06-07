#include "aegisflow/app/risk_service.hpp"
#include <chrono>
#include <cstdint>
#include <string>

namespace aegisflow::app {

namespace {

uint64_t nowMills() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()
        ).count()
    );
}

uint64_t elapseMicros(std::chrono::steady_clock::time_point start) {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(
            steady_clock::now() - start
        ).count()
    );
}

std::string formatRecentAction(const aegisflow::feature::RecentAction& action) {
    return aegisflow::v1::EventType_Name(action.type) + 
    ":" + 
    aegisflow::v1::EventResult_Name(action.result) + 
    "@" +
    std::to_string(action.timestamp_ms);
}

void fillFeatureSnapshot(
    const aegisflow::feature::FeatureSnapshot& snapshot,
    aegisflow::v1::FeatureSnapshot* pb_snapshot
) {
    pb_snapshot->set_user_id(snapshot.user_id);
    pb_snapshot->set_user_login_1m(snapshot.user_login_1m);
    pb_snapshot->set_user_login_5m(snapshot.user_login_5m);
    pb_snapshot->set_user_login_1h(snapshot.user_login_1h);
    pb_snapshot->set_user_login_fail_5m(snapshot.user_login_fail_5m);

    for (const auto& action : snapshot.recent_actions) {
        pb_snapshot->add_recent_actions(formatRecentAction(action));
    }
}

} //namespace

// 处理事件
aegisflow::v1::ReportEventResponse RiskService::handleEvent(
    const aegisflow::v1::ReportEventRequest& request
) {
    const auto start = std::chrono::steady_clock::now();
    const uint64_t now_ms = nowMills();

    aegisflow::v1::ReportEventResponse response;

    const auto& event = request.event();
    auto* decision = response.mutable_decision();

    const auto snapshot = feature_store_.updateAndGet(event, now_ms);

    decision->set_event_id(event.event_id());
    decision->set_user_id(event.user_id());

    if (event.user_id().empty()) {
        decision->set_action(aegisflow::v1::REVIEW);
        decision->set_risk_score(10);
    
        auto* reason = decision->add_reasons();
        reason->set_code("invalid_user_id");
        reason->set_message("empty user_id");
        reason->set_severity(1);
    } else {
        decision->set_action(aegisflow::v1::PASS);
        decision->set_risk_score(0);
    }

    fillFeatureSnapshot(snapshot, decision->mutable_features());

    decision->set_cost_us(elapseMicros(start));

    return response;
}

} // namespace aegisflow::app
