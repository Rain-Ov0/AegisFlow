#include "aegisflow/net/login_business_handler.hpp"

#include "aegisflow/app/login_request_validator.hpp"
#include "aegisflow/net/protocol_contract.hpp"

#include "login.pb.h"

#include <google/protobuf/arena.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegisflow::net {

namespace {

std::uint64_t nowMillis() noexcept {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()
        ).count()
    );
}

aegisflow::login::DecisionAction toProtoAction(
    const aegisflow::domain::LoginDecisionAction action
) {
    switch (action) {
    case aegisflow::domain::LoginDecisionAction::pass:
        return aegisflow::login::PASS;
    case aegisflow::domain::LoginDecisionAction::review:
        return aegisflow::login::REVIEW;
    case aegisflow::domain::LoginDecisionAction::reject:
        return aegisflow::login::REJECT;
    }
    return aegisflow::login::DECISION_ACTION_UNKNOWN;
}

void fillProtoDecision(
    const aegisflow::domain::LoginDecision& source,
    aegisflow::login::LoginDecision* target
) {
    target->set_attempt_id(source.attempt_id);
    target->set_user_id(source.user_id);
    target->set_action(toProtoAction(source.action));
    target->set_risk_score(source.risk_score);
    target->set_cost_us(source.cost_us);

    for (const auto& source_hit : source.policy_hits) {
        auto* target_hit = target->add_policy_hits();
        target_hit->set_reason_code(source_hit.reason_code);
        target_hit->set_action(toProtoAction(source_hit.action));
        target_hit->set_risk_score(source_hit.risk_score);
    }
}

FrameBusinessResult resultWithStatus(
    const FrameBusinessStatus status
) {
    FrameBusinessResult result;
    result.status = status;
    return result;
}

std::vector<std::uint8_t> encodeLoginResponseFrame(
    const aegisflow::login::LoginResponse& response
) {
    std::string payload;
    if (!response.SerializeToString(&payload) || payload.empty() ||
        payload.size() > protocol::kMaxPayloadSize) {
        throw std::runtime_error("构造服务响应失败");
    }

    const auto header = protocol::encodePayloadLength(
        static_cast<std::uint32_t>(payload.size())
    );
    std::vector<std::uint8_t> frame;
    frame.reserve(header.size() + payload.size());
    frame.insert(frame.end(), header.begin(), header.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

}  // 命名空间

LoginBusinessHandler::LoginBusinessHandler(
    std::shared_ptr<app::RiskService> risk_service,
    std::shared_ptr<app::BlacklistCandidateQueue> candidate_queue
) : risk_service_(std::move(risk_service)),
    candidate_queue_(std::move(candidate_queue)) {
    if (risk_service_ == nullptr) {
        throw std::invalid_argument("登录业务处理器缺少 RiskService");
    }
}

FrameBusinessResult LoginBusinessHandler::handle(
    const base::ArrayView<const std::uint8_t> request_payload,
    const runtime::CancellationToken stop_token
) {
    if (stop_token.stopRequested()) {
        return resultWithStatus(FrameBusinessStatus::Cancelled);
    }

    // Arena 归当前 Worker 线程独占，每个任务前后重置，避免请求间保留 Proto 对象。
    thread_local google::protobuf::Arena arena;
    arena.Reset();
    FrameBusinessResult result;
    try {
        auto* request = google::protobuf::Arena::CreateMessage<
            aegisflow::login::LoginRequest>(&arena);
        if (!request->ParseFromArray(
                request_payload.data(),
                static_cast<int>(request_payload.size()))) {
            result.status = FrameBusinessStatus::InvalidRequest;
        } else {
            const auto attempt = app::LoginRequestValidator::validate(
                *request, nowMillis());
            if (!attempt.has_value()) {
                result.status = FrameBusinessStatus::InvalidRequest;
            } else if (stop_token.stopRequested()) {
                result.status = FrameBusinessStatus::Cancelled;
            } else {
                auto* response = google::protobuf::Arena::CreateMessage<
                    aegisflow::login::LoginResponse>(&arena);
                auto evaluation = risk_service_->evaluate(*attempt);
                if (candidate_queue_ != nullptr) {
                    // 领域计算完成后再尝试提交候选；队列满不改写风险决策。
                    for (auto& candidate : evaluation.candidates) {
                        (void)candidate_queue_->trySubmit(
                            std::move(candidate));
                    }
                }
                response->Clear();
                response->set_status(
                    aegisflow::login::RESPONSE_STATUS_OK);
                fillProtoDecision(
                    evaluation.decision,
                    response->mutable_decision());
                std::string serialized;
                if (!response->SerializeToString(&serialized) ||
                    serialized.empty()) {
                    result.status = FrameBusinessStatus::Failed;
                } else {
                    result.status = FrameBusinessStatus::Response;
                    // 跨线程 completion 携带自有字节，不引用 Arena 内存。
                    result.response_payload.assign(
                        serialized.begin(), serialized.end());
                }
            }
        }
    } catch (...) {
        result = resultWithStatus(FrameBusinessStatus::Failed);
    }
    arena.Reset();
    return result;
}

std::vector<std::uint8_t> buildLoginOverloadResponseFrame() {
    aegisflow::login::LoginResponse response;
    response.set_status(
        aegisflow::login::RESPONSE_STATUS_OVERLOADED
    );
    response.set_reason_code("service_overloaded");

    return encodeLoginResponseFrame(response);
}

std::vector<std::uint8_t> buildLoginBusinessTimeoutResponseFrame() {
    aegisflow::login::LoginResponse response;
    response.set_status(aegisflow::login::RESPONSE_STATUS_TIMEOUT);
    response.set_reason_code("business_timeout");
    return encodeLoginResponseFrame(response);
}

}  // 命名空间 aegisflow::net
