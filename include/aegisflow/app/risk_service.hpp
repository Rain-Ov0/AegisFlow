#pragma once

#include "event.pb.h"
#include "decision.pb.h"
#include "aegisflow/feature/feature_store.hpp"
#include "aegisflow/runtime/worker_pool.hpp"
#include "aegisflow/rule/decision_aggregator.hpp"
#include "aegisflow/rule/rule_engine.hpp"


namespace aegisflow::app {

class RiskService {
public: 
    explicit RiskService(size_t worker_num = 0,
        const std::string rule_file = "config/rules.dsl"
    );

       aegisflow::v1::ReportEventResponse handleEvent(
        const aegisflow::v1::ReportEventRequest& request 
    );

private:
    std::shared_ptr<const aegisflow::rule::RuleSet> rule_set_;
    aegisflow::rule::RuleEngine rule_engine_;
    aegisflow::rule::DecisionAggregator decision_aggregator_;

    aegisflow::feature::FeatureStore feature_store_;
    aegisflow::runtime::WorkerPool worker_pool_;
};

} //namespace aegisflow::app
