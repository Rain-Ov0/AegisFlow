#include "aegisflow/rule/decision_aggregator.hpp"

namespace aegisflow::rule {

DecisionResult DecisionAggregator::aggregate(
    const std::vector<RuleHit>& hits
) const {
    DecisionResult result;

    for (const auto& hit : hits) {
        result.hit_rule_ids.push_back(hit.rule_id);

        if (!hit.reason_code.empty()) {
            result.reasons.push_back(hit.reason_code);
        }

        if (severity(hit.action) > severity(result.action)) {
            result.action = hit.action;
        }

        result.risk_score += score(hit.action);
    }

    return result;
}

int DecisionAggregator::severity(DecisionAction action) {
    switch (action) {
    case DecisionAction::Pass:
        return 0;
    case DecisionAction::Review:
        return 1;
    case DecisionAction::Reject:
        return 2;
    }

    return 0;
}

int DecisionAggregator::score(DecisionAction action) {
    switch (action) {
    case DecisionAction::Pass:
        return 0;
    case DecisionAction::Review:
        return 30;
    case DecisionAction::Reject:
        return 100;
    }

    return 0;
}

} // namespace aegisflow::rule