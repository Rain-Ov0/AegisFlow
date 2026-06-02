#pragma once

#include "event.pb.h"
#include "decision.pb.h"

namespace aegisflow::app {

class RiskService {
public: 
    aegisflow::v1::ReportEventResponse handleEvent(
        const aegisflow::v1::ReportEventRequest& request 
    );
};

} //namespace aegisflow::app
