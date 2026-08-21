#include "aegisflow/app/login_request_validator.hpp"

#include "aegisflow/domain/ip_address.hpp"

namespace aegisflow::app {

std::optional<aegisflow::domain::LoginAttempt>
LoginRequestValidator::validate(
    const aegisflow::login::LoginRequest& request,
    const std::uint64_t now_ms
) {
    // optional scalar 的 has_* 才能区分“调用方显式传 0”和“字段缺失”。
    // attempt_id 参与请求响应相关性校验，因此缺失时不能默认为 0。
    if (!request.has_attempt_id() || !request.has_user_id() ||
        request.user_id().empty() ||
        request.user_id().size() > kMaxUserIdBytes || !request.has_ip() ||
        request.ip().empty() || request.ip().size() > kMaxIpBytes ||
        !request.has_device_id() ||
        request.device_id().empty() ||
        request.device_id().size() > kMaxDeviceIdBytes ||
        !request.has_timestamp_ms() || request.timestamp_ms() > now_ms ||
        now_ms - request.timestamp_ms() >= kMaxPastAgeMs ||
        !request.has_result()) {
        return std::nullopt;
    }

    const auto canonical_ip = aegisflow::domain::IpAddress::parse(request.ip());
    if (!canonical_ip.has_value()) {
        return std::nullopt;
    }

    const int result_value = static_cast<int>(request.result());
    if (!aegisflow::login::LoginResult_IsValid(result_value) ||
        request.result() == aegisflow::login::LOGIN_RESULT_UNKNOWN) {
        return std::nullopt;
    }

    const auto result = request.result() == aegisflow::login::SUCCESS
                            ? aegisflow::domain::LoginResult::success
                            : aegisflow::domain::LoginResult::fail;
    return aegisflow::domain::LoginAttempt(
        request.attempt_id(),
        request.timestamp_ms(),
        request.user_id(),
        canonical_ip->canonicalText(),
        request.device_id(),
        result
    );
}

}  // 命名空间 aegisflow::app
