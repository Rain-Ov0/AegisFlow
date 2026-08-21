#include "aegisflow/app/login_request_validator.hpp"

#include "tests/support/test_harness.hpp"

#include "login.pb.h"

#include <cstdint>
#include <string>

namespace {

using aegisflow::test::require;

aegisflow::login::LoginRequest validRequest(const std::uint64_t now_ms) {
    aegisflow::login::LoginRequest request;
    request.set_attempt_id(7);
    request.set_timestamp_ms(now_ms);
    request.set_user_id("user-7");
    request.set_ip("2001:db8::7");
    request.set_device_id("device-7");
    request.set_result(aegisflow::login::FAIL);
    return request;
}

bool accepted(
    const aegisflow::login::LoginRequest& request,
    const std::uint64_t now_ms
) {
    return aegisflow::app::LoginRequestValidator::validate(request, now_ms)
        .has_value();
}

void requiredFieldsAreEnforced() {
    constexpr std::uint64_t now_ms = 10'000'000;
    require(accepted(validRequest(now_ms), now_ms), "规范请求必须通过校验");

    auto request = validRequest(now_ms);
    request.clear_attempt_id();
    require(!accepted(request, now_ms), "缺失 attempt_id 必须被拒绝");

    request = validRequest(now_ms);
    request.clear_user_id();
    require(!accepted(request, now_ms), "缺失 user_id 必须被拒绝");

    request = validRequest(now_ms);
    request.clear_ip();
    require(!accepted(request, now_ms), "缺失 IP 必须被拒绝");

    request = validRequest(now_ms);
    request.clear_device_id();
    require(!accepted(request, now_ms), "缺失 device_id 必须被拒绝");

    request = validRequest(now_ms);
    request.clear_timestamp_ms();
    require(!accepted(request, now_ms), "缺失 timestamp_ms 必须被拒绝");

    request = validRequest(now_ms);
    request.clear_result();
    require(!accepted(request, now_ms), "缺失 result 必须被拒绝");
}

void fieldBoundariesAreEnforced() {
    using aegisflow::app::LoginRequestValidator;
    constexpr std::uint64_t now_ms = 10'000'000;

    auto request = validRequest(now_ms);
    request.set_user_id(std::string(LoginRequestValidator::kMaxUserIdBytes, 'u'));
    request.set_device_id(
        std::string(LoginRequestValidator::kMaxDeviceIdBytes, 'd'));
    require(accepted(request, now_ms), "ID 长度上限应可接受");

    request.set_user_id(
        std::string(LoginRequestValidator::kMaxUserIdBytes + 1, 'u'));
    require(!accepted(request, now_ms), "超长 user_id 必须被拒绝");

    request = validRequest(now_ms);
    request.set_device_id(
        std::string(LoginRequestValidator::kMaxDeviceIdBytes + 1, 'd'));
    require(!accepted(request, now_ms), "超长 device_id 必须被拒绝");

    request = validRequest(now_ms);
    request.set_timestamp_ms(now_ms + 1);
    require(!accepted(request, now_ms), "未来时间必须被拒绝");

    request.set_timestamp_ms(now_ms - LoginRequestValidator::kMaxPastAgeMs + 1);
    require(accepted(request, now_ms), "历史时间窗口内请求应可接受");
    request.set_timestamp_ms(now_ms - LoginRequestValidator::kMaxPastAgeMs);
    require(!accepted(request, now_ms), "达到历史时间上限的请求必须被拒绝");
}

void ipAndResultDomainsAreEnforced() {
    constexpr std::uint64_t now_ms = 10'000'000;
    auto request = validRequest(now_ms);

    request.set_ip("192.0.2.7");
    require(accepted(request, now_ms), "规范 IPv4 必须通过校验");
    request.set_ip("2001:db8:0:0::7");
    require(accepted(request, now_ms), "有效 IPv6 必须通过校验");
    request.set_ip("::ffff:192.0.2.7");
    require(accepted(request, now_ms), "IPv4-mapped IPv6 必须通过校验");
    request.set_ip("not-an-ip");
    require(!accepted(request, now_ms), "非法 IP 必须被拒绝");

    request = validRequest(now_ms);
    request.set_ip(std::string("192.0.2.7\0suffix", 16));
    require(!accepted(request, now_ms), "嵌入 NUL 的 IP 必须被拒绝");

    request = validRequest(now_ms);
    request.set_result(aegisflow::login::LOGIN_RESULT_UNKNOWN);
    require(!accepted(request, now_ms), "未知登录结果必须被拒绝");
}

void ipIsCanonicalizedBeforeEnteringDomain() {
    constexpr std::uint64_t now_ms = 10'000'000;

    auto request = validRequest(now_ms);
    request.set_ip("2001:0db8:0000:0000:0000:0000:0000:0007");
    auto attempt = aegisflow::app::LoginRequestValidator::validate(
        request, now_ms);
    require(attempt.has_value(), "完整 IPv6 文本必须通过校验");
    require(
        attempt->ip() == "2001:db8::7",
        "IPv6 进入领域层前必须压缩为规范文本"
    );

    request.set_ip("::ffff:192.0.2.7");
    attempt = aegisflow::app::LoginRequestValidator::validate(request, now_ms);
    require(attempt.has_value(), "IPv4-mapped IPv6 必须通过校验");
    require(
        attempt->ip() == "192.0.2.7",
        "IPv4-mapped IPv6 必须与普通 IPv4 共用同一领域 key"
    );
}

void validatedRequestBecomesDomainValue() {
    constexpr std::uint64_t now_ms = 10'000'000;
    const auto attempt = aegisflow::app::LoginRequestValidator::validate(
        validRequest(now_ms),
        now_ms
    );
    require(attempt.has_value(), "规范请求必须产生领域值");
    require(
        attempt->attemptId() == 7 && attempt->timestampMs() == now_ms &&
            attempt->userId() == "user-7" && attempt->ip() == "2001:db8::7" &&
            attempt->deviceId() == "device-7" &&
            attempt->result() == aegisflow::domain::LoginResult::fail,
        "领域值必须完整保留已校验请求"
    );
}

}  // namespace

int main() {
    return aegisflow::test::runModule(
        "login_request_validator",
        {
            {"必填字段", requiredFieldsAreEnforced},
            {"字段边界", fieldBoundariesAreEnforced},
            {"IP 与枚举域", ipAndResultDomainsAreEnforced},
            {"IP 规范化", ipIsCanonicalizedBeforeEnteringDomain},
            {"领域值映射", validatedRequestBecomesDomainValue},
        }
    );
}
