#include "aegisflow/app/risk_service.hpp"

namespace aegisflow::app {

aegisflow::v1::ReportEventResponse RiskService::handleEvent(
    const aegisflow::v1::ReportEventRequest& request
) {
    aegisflow::v1::ReportEventResponse response;

    const auto& event = request.event();
    auto* decision = response.mutable_decision();

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
    return response;
}

} // namespace aegisflow::app
