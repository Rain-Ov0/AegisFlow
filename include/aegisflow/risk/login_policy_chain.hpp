#pragma once

#include "aegisflow/domain/login.hpp"
#include "aegisflow/feature/user_state.hpp"
#include "aegisflow/risk/login_policy.hpp"

namespace aegisflow::risk {

class LoginPolicyChain final {
public:
    explicit LoginPolicyChain(const LoginPolicyConfig& config);

    LoginPolicyChain(const LoginPolicyChain&) = delete;
    LoginPolicyChain& operator=(const LoginPolicyChain&) = delete;
    LoginPolicyChain(LoginPolicyChain&&) noexcept = default;
    LoginPolicyChain& operator=(LoginPolicyChain&&) noexcept = default;

    [[nodiscard]] aegisflow::domain::LoginDecision evaluate(
        const aegisflow::domain::LoginAttempt& attempt,
        const aegisflow::feature::LoginFeatureSnapshot& features,
        const LoginBlacklistMatches& blacklist
    ) const;

private:
    LoginPolicyConfig config_;
};

}  // 命名空间 aegisflow::risk
