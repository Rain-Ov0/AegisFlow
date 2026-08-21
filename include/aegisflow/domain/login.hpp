#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace aegisflow::app {
class LoginRequestValidator;
}

namespace aegisflow::domain {

enum class LoginResult {
    success,
    fail,
};

enum class LoginDecisionAction {
    pass,
    review,
    reject,
};

struct PolicyHit {
    std::string reason_code;
    LoginDecisionAction action = LoginDecisionAction::pass;
    std::int32_t risk_score = 0;
};

struct LoginDecision {
    std::uint64_t attempt_id = 0;
    std::string user_id;
    LoginDecisionAction action = LoginDecisionAction::pass;
    std::int32_t risk_score = 0;
    std::vector<PolicyHit> policy_hits;
    std::uint64_t cost_us = 0;
};

// 该类型只能由集中校验器构造，业务层接收到的实例天然通过输入校验。
class LoginAttempt final {
public:
    LoginAttempt(const LoginAttempt&) = default;
    LoginAttempt(LoginAttempt&&) noexcept = default;
    LoginAttempt& operator=(const LoginAttempt&) = default;
    LoginAttempt& operator=(LoginAttempt&&) noexcept = default;

    [[nodiscard]] std::uint64_t attemptId() const noexcept {
        return attempt_id_;
    }

    [[nodiscard]] std::uint64_t timestampMs() const noexcept {
        return timestamp_ms_;
    }

    [[nodiscard]] const std::string& userId() const noexcept {
        return user_id_;
    }

    [[nodiscard]] const std::string& ip() const noexcept {
        return ip_;
    }

    [[nodiscard]] const std::string& deviceId() const noexcept {
        return device_id_;
    }

    [[nodiscard]] LoginResult result() const noexcept {
        return result_;
    }

private:
    LoginAttempt(
        std::uint64_t attempt_id,
        std::uint64_t timestamp_ms,
        std::string user_id,
        std::string ip,
        std::string device_id,
        LoginResult result
    )
        : attempt_id_(attempt_id),
          timestamp_ms_(timestamp_ms),
          user_id_(std::move(user_id)),
          ip_(std::move(ip)),
          device_id_(std::move(device_id)),
          result_(result) {}

    friend class aegisflow::app::LoginRequestValidator;

    std::uint64_t attempt_id_ = 0;
    std::uint64_t timestamp_ms_ = 0;
    std::string user_id_;
    std::string ip_;
    std::string device_id_;
    LoginResult result_ = LoginResult::success;
};

}  // 命名空间 aegisflow::domain
