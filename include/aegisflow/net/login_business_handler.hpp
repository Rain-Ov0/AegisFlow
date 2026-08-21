#pragma once

#include "aegisflow/base/array_view.hpp"
#include "aegisflow/app/blacklist_candidate_queue.hpp"
#include "aegisflow/app/risk_service.hpp"
#include "aegisflow/net/event_loop.hpp"
#include "aegisflow/runtime/cancellation.hpp"

#include <memory>
#include <vector>

namespace aegisflow::net {

class LoginBusinessHandler final : public IFrameBusinessHandler {
public:
    explicit LoginBusinessHandler(
        std::shared_ptr<app::RiskService> risk_service,
        std::shared_ptr<app::BlacklistCandidateQueue> candidate_queue = nullptr
    );

    [[nodiscard]] FrameBusinessResult handle(
        base::ArrayView<const std::uint8_t> request_payload,
        runtime::CancellationToken stop_token
    ) override;

private:
    std::shared_ptr<app::RiskService> risk_service_;
    std::shared_ptr<app::BlacklistCandidateQueue> candidate_queue_;
};

[[nodiscard]] std::vector<std::uint8_t> buildLoginOverloadResponseFrame();
[[nodiscard]] std::vector<std::uint8_t>
buildLoginBusinessTimeoutResponseFrame();

}  // 命名空间 aegisflow::net
