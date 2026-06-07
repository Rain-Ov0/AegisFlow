#pragma once

#include "event.pb.h"
#include "decision.pb.h"
#include "aegisflow/feature/feature_store.hpp"

namespace aegisflow::app {

class RiskService {
public: 
    aegisflow::v1::ReportEventResponse handleEvent(
        const aegisflow::v1::ReportEventRequest& request 
    );

private:
    aegisflow::feature::FeatureStore feature_store_;
};

} //namespace aegisflow::app
