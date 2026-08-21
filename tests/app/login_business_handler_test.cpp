#include "aegisflow/app/blacklist_candidate_queue.hpp"
#include "aegisflow/app/risk_service.hpp"
#include "aegisflow/net/login_business_handler.hpp"
#include "aegisflow/risk/blacklist_manager.hpp"

#include "tests/support/test_harness.hpp"

#include "login.pb.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using aegisflow::test::require;

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::vector<std::uint8_t> requestPayload(
    const std::uint64_t attempt_id,
    const std::string& user_id
) {
    aegisflow::login::LoginRequest request;
    request.set_attempt_id(attempt_id);
    request.set_timestamp_ms(nowMs());
    request.set_user_id(user_id);
    request.set_ip("2001:0db8:0:0:0:0:0:77");
    request.set_device_id("shared-device");
    request.set_result(aegisflow::login::FAIL);
    std::string payload;
    require(request.SerializeToString(&payload), "测试请求应可序列化");
    return {payload.begin(), payload.end()};
}

aegisflow::login::LoginResponse decode(
    const aegisflow::net::FrameBusinessResult& result
) {
    require(result.status == aegisflow::net::FrameBusinessStatus::Response,
            "业务处理应返回 Protobuf payload");
    aegisflow::login::LoginResponse response;
    require(response.ParseFromArray(
                result.response_payload.data(),
                static_cast<int>(result.response_payload.size())),
            "响应 payload 应可解析");
    return response;
}

bool hasCredentialStuffingHit(
    const aegisflow::login::LoginResponse& response
) {
    for (const auto& hit : response.decision().policy_hits()) {
        if (hit.reason_code() == "credential_stuffing_attack" &&
            hit.action() == aegisflow::login::REJECT) {
            return true;
        }
    }
    return false;
}

void rejectCandidatesReachSharedQueueAndFullKeepsDecision() {
    aegisflow::risk::LoginPolicyConfig policy;
    policy.user_failure_review_threshold = 100;
    policy.ip_spray_review_threshold = 1;
    policy.device_sharing_review_threshold = 100;
    policy.ip_distinct_reject_threshold = 1;
    policy.ip_failure_reject_threshold = 1;
    aegisflow::risk::BlacklistManager manager;
    auto service = std::make_shared<aegisflow::app::RiskService>(
        policy, &manager);
    auto candidates =
        std::make_shared<aegisflow::app::BlacklistCandidateQueue>(1);
    aegisflow::net::LoginBusinessHandler handler(service, candidates);

    const auto first_payload = requestPayload(1, "attack-user-1");
    const auto first = decode(handler.handle(first_payload, {}));
    require(first.status() == aegisflow::login::RESPONSE_STATUS_OK &&
                first.decision().action() == aegisflow::login::REJECT &&
                hasCredentialStuffingHit(first),
            "真实 RiskService 链应在撞库阈值上返回 REJECT");
    const auto after_first = candidates->stats();
    require(after_first.queued == 1 && after_first.dropped == 0,
            "credential stuffing REJECT 应把 IP 候选提交到共享队列");

    const auto second_payload = requestPayload(2, "attack-user-2");
    const auto second = decode(handler.handle(second_payload, {}));
    const auto after_second = candidates->stats();
    require(second.status() == aegisflow::login::RESPONSE_STATUS_OK &&
                second.decision().action() == aegisflow::login::REJECT &&
                hasCredentialStuffingHit(second),
            "候选队列满不得把已计算的 REJECT 改写为错误");
    require(after_second.queued == 1 && after_second.dropped == 1,
            "队列满时应保持有界并精确增加 dropped counter");
}

}  // 命名空间

int main() {
    return aegisflow::test::runModule(
        "login_business_handler",
        {{"REJECT 候选与 queue-full 决策不变",
          rejectCandidatesReachSharedQueueAndFullKeepsDecision}});
}
