#pragma once

#include "aegisflow/feature/feature_store.hpp"
#include "aegisflow/rule/decision_aggregator.hpp"
#include "aegisflow/rule/rule_engine.hpp"
#include "aegisflow/runtime/worker_pool.hpp"
#include "decision.pb.h"
#include "event.pb.h"

#include <cstddef>
#include <memory>
#include <string>

namespace aegisflow::risk {
class BlacklistManager;
}

namespace aegisflow::app {

class RiskService {
public:
    explicit RiskService(
        size_t worker_num = 0,
        std::string rule_file = "config/rules.dsl",
        aegisflow::risk::BlacklistManager* blacklist_manager = nullptr
    );

    aegisflow::v1::ReportEventResponse handleEvent(
        const aegisflow::v1::ReportEventRequest& request
    );

private:
    std::shared_ptr<const aegisflow::rule::RuleSet> rule_set_;
    aegisflow::rule::RuleEngine rule_engine_;
    aegisflow::rule::DecisionAggregator decision_aggregator_;

    aegisflow::risk::BlacklistManager* blacklist_manager_ = nullptr;

    aegisflow::feature::FeatureStore feature_store_;
    aegisflow::runtime::WorkerPool worker_pool_;
};

} // namespace aegisflow::app