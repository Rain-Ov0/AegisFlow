#pragma once

#include "event.pb.h"
#include "decision.pb.h"
#include "aegisflow/feature/feature_store.hpp"
#include "aegisflow/runtime/worker_pool.hpp"

namespace aegisflow::app {

class RiskService {
public: 
    explicit RiskService(size_t worker_num = 0);

       aegisflow::v1::ReportEventResponse handleEvent(
        const aegisflow::v1::ReportEventRequest& request 
    );

private:
    aegisflow::feature::FeatureStore feature_store_;
    aegisflow::runtime::WorkerPool worker_pool_;
};

} //namespace aegisflow::app
