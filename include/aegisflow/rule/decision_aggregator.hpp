#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "aegisflow/rule/rule_engine.hpp"

namespace aegisflow::rule {

struct DecisionResult {
    DecisionAction action = DecisionAction::Pass;
    int risk_score = 0;
    std::vector<uint64_t> hit_rule_ids;
    std::vector<std::string> reasons;
};

class DecisionAggregator {
public:
    DecisionResult aggregate(const std::vector<RuleHit>& hits) const;

private:
    static int severity(DecisionAction action);
    static int score(DecisionAction action);
};

} // namespace aegisflow::rule