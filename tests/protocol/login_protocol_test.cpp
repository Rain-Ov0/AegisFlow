#include "aegisflow/benchmark/login_frame_codec.hpp"
#include "aegisflow/base/array_view.hpp"
#include "aegisflow/net/protocol_contract.hpp"

#include "tests/support/test_harness.hpp"

#include "login.pb.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using aegisflow::test::require;
namespace benchmark = aegisflow::benchmark;
namespace protocol = aegisflow::net::protocol;

std::vector<std::uint8_t> responseFrame(
    const aegisflow::login::LoginResponse& response
) {
    const auto payload = response.SerializeAsString();
    const auto header = protocol::encodePayloadLength(
        static_cast<std::uint32_t>(payload.size())
    );
    std::vector<std::uint8_t> frame(header.begin(), header.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

void requestFramePreservesLengthAndProtobuf() {
    aegisflow::login::LoginRequest source;
    source.set_attempt_id(42);
    source.set_timestamp_ms(123456);
    source.set_user_id("user-42");
    source.set_ip("2001:db8::42");
    source.set_device_id("device-42");
    source.set_result(aegisflow::login::FAIL);

    const auto frame = benchmark::encodeLoginRequestFrame(source);
    require(frame.size() > protocol::kFrameHeaderSize, "请求帧必须包含载荷");
    protocol::FrameHeader header{};
    std::copy_n(frame.begin(), header.size(), header.begin());
    const auto payload_size = protocol::decodePayloadLength(header);
    require(
        frame.size() == protocol::kFrameHeaderSize + payload_size,
        "请求帧长度头必须只计算 Protobuf payload"
    );

    aegisflow::login::LoginRequest decoded;
    const auto payload = aegisflow::base::ArrayView<const std::uint8_t>(frame)
                             .subview(protocol::kFrameHeaderSize);
    require(
        decoded.ParseFromArray(payload.data(), payload.size()),
        "请求帧中的 Protobuf 必须可解析"
    );
    require(
        decoded.has_attempt_id() && decoded.attempt_id() == 42 &&
            decoded.has_timestamp_ms() && decoded.timestamp_ms() == 123456 &&
            decoded.has_user_id() && decoded.user_id() == "user-42" &&
            decoded.has_ip() && decoded.ip() == "2001:db8::42" &&
            decoded.has_device_id() && decoded.device_id() == "device-42" &&
            decoded.has_result() && decoded.result() == aegisflow::login::FAIL,
        "请求 Protobuf 往返后字段或 presence 不一致"
    );
}

void responseFrameParsesDecisionAndStatus() {
    aegisflow::login::LoginResponse source;
    source.set_status(aegisflow::login::RESPONSE_STATUS_OK);
    auto* decision = source.mutable_decision();
    decision->set_attempt_id(7);
    decision->set_user_id("user-7");
    decision->set_action(aegisflow::login::REJECT);
    decision->set_risk_score(100);
    auto* hit = decision->add_policy_hits();
    hit->set_reason_code("credential_stuffing_attack");
    hit->set_action(aegisflow::login::REJECT);
    hit->set_risk_score(100);

    const auto frame = responseFrame(source);
    const auto decoded = benchmark::decodeLoginResponseFrame(frame);
    require(decoded.ok(), "规范响应帧必须可解析");
    require(
        decoded.response.status() == aegisflow::login::RESPONSE_STATUS_OK &&
            decoded.response.has_decision() &&
            decoded.response.decision().attempt_id() == 7 &&
            decoded.response.decision().action() == aegisflow::login::REJECT &&
            decoded.response.decision().policy_hits_size() == 1,
        "响应帧解析后的决策或状态不一致"
    );
}

void invalidAndTruncatedFramesAreRejectedBeforeParsing() {
    const auto empty_header = protocol::encodePayloadLength(0);
    auto decoded = benchmark::decodeLoginResponseFrame(empty_header);
    require(
        decoded.error == benchmark::LoginFrameError::InvalidPayloadLength,
        "零长度响应帧必须作为协议错误拒绝"
    );

    const auto oversized = protocol::encodePayloadLength(
        protocol::kMaxPayloadSize + 1
    );
    decoded = benchmark::decodeLoginResponseFrame(oversized);
    require(
        decoded.error == benchmark::LoginFrameError::InvalidPayloadLength,
        "超长响应帧必须在分配 payload 前拒绝"
    );

    constexpr std::array<std::uint8_t, 2> half_header = {0, 0};
    decoded = benchmark::decodeLoginResponseFrame(half_header);
    require(
        decoded.error == benchmark::LoginFrameError::TruncatedFrame,
        "截断帧头必须拒绝"
    );

    std::vector<std::uint8_t> half_payload = {0, 0, 0, 3, 0x08};
    decoded = benchmark::decodeLoginResponseFrame(half_payload);
    require(
        decoded.error == benchmark::LoginFrameError::TruncatedFrame,
        "截断 payload 必须拒绝"
    );
}

void nonProtobufAndTrailingBytesAreRejected() {
    // LoginResponse.decision 声明长度为 2，实际只有 1 字节。
    std::vector<std::uint8_t> malformed = {0, 0, 0, 3, 0x0A, 0x02, 0x08};
    auto decoded = benchmark::decodeLoginResponseFrame(malformed);
    require(
        decoded.error == benchmark::LoginFrameError::ProtobufParse,
        "非 Protobuf 响应 payload 必须解析失败"
    );

    aegisflow::login::LoginResponse response;
    response.set_status(aegisflow::login::RESPONSE_STATUS_OVERLOADED);
    auto frame = responseFrame(response);
    frame.push_back(0xff);
    decoded = benchmark::decodeLoginResponseFrame(frame);
    require(
        decoded.error == benchmark::LoginFrameError::TrailingBytes,
        "一帧后的额外字节不得被静默吞掉"
    );
}

}  // namespace

int main() {
    return aegisflow::test::runModule(
        "login_protocol",
        {
            {"请求帧字节往返", requestFramePreservesLengthAndProtobuf},
            {"响应帧字节往返", responseFrameParsesDecisionAndStatus},
            {"非法长度与截断", invalidAndTruncatedFramesAreRejectedBeforeParsing},
            {"非法 Protobuf 与尾随字节", nonProtobufAndTrailingBytesAreRejected},
        }
    );
}
