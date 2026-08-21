#pragma once

#include "aegisflow/base/array_view.hpp"

#include "login.pb.h"

#include <cstdint>
#include <vector>

namespace aegisflow::benchmark {

enum class LoginFrameError : std::uint8_t {
    None,
    InvalidPayloadLength,
    TruncatedFrame,
    TrailingBytes,
    ProtobufParse,
};

struct LoginResponseFrame {
    LoginFrameError error = LoginFrameError::None;
    aegisflow::login::LoginResponse response;

    [[nodiscard]] bool ok() const noexcept {
        return error == LoginFrameError::None;
    }
};

// benchmark 的协议边界只处理完整自有字节：请求编码成一帧，响应从一帧
// 恢复为 LoginResponse。socket 的超时和完整读取由负载客户端负责。
[[nodiscard]] std::vector<std::uint8_t> encodeLoginRequestFrame(
    const aegisflow::login::LoginRequest& request
);

[[nodiscard]] LoginResponseFrame decodeLoginResponseFrame(
    base::ArrayView<const std::uint8_t> frame
);

}  // namespace aegisflow::benchmark
