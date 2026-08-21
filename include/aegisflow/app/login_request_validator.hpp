#pragma once

#include "aegisflow/domain/login.hpp"

#include "login.pb.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace aegisflow::app {

class LoginRequestValidator final {
public:
    static constexpr std::size_t kMaxUserIdBytes = 128;
    static constexpr std::size_t kMaxDeviceIdBytes = 128;
    static constexpr std::size_t kMaxIpBytes = 45;
    static constexpr std::uint64_t kMaxPastAgeMs = 60ULL * 60ULL * 1000ULL;

    [[nodiscard]] static std::optional<aegisflow::domain::LoginAttempt>
    validate(
        const aegisflow::login::LoginRequest& request,
        std::uint64_t now_ms
    );
};

}  // 命名空间 aegisflow::app
