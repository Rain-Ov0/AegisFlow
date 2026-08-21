#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace aegisflow::net::protocol {

// 线协议格式：[4 字节大端序载荷长度][载荷字节]。
// 编码长度只计算载荷，不包含头部本身。
inline constexpr std::size_t kFrameHeaderSize = 4;
inline constexpr std::uint32_t kMinPayloadSize = 1;
inline constexpr std::uint32_t kMaxPayloadSize = 1024U * 1024U;

static_assert(std::numeric_limits<std::uint32_t>::digits == 32);

using FrameHeader = std::array<std::uint8_t, kFrameHeaderSize>;

enum class ProtocolError {
    none,
    empty_payload,
    payload_too_large,
    multiple_requests,
};

[[nodiscard]] constexpr FrameHeader encodePayloadLength(
    const std::uint32_t payload_length
) noexcept {
    return {
        static_cast<std::uint8_t>((payload_length >> 24U) & 0xffU),
        static_cast<std::uint8_t>((payload_length >> 16U) & 0xffU),
        static_cast<std::uint8_t>((payload_length >> 8U) & 0xffU),
        static_cast<std::uint8_t>(payload_length & 0xffU),
    };
}

[[nodiscard]] constexpr std::uint32_t decodePayloadLength(
    const FrameHeader& header
) noexcept {
    return (static_cast<std::uint32_t>(header[0]) << 24U) |
           (static_cast<std::uint32_t>(header[1]) << 16U) |
           (static_cast<std::uint32_t>(header[2]) << 8U) |
           static_cast<std::uint32_t>(header[3]);
}

[[nodiscard]] constexpr ProtocolError validatePayloadLength(
    const std::uint32_t payload_length
) noexcept {
    if (payload_length < kMinPayloadSize) {
        return ProtocolError::empty_payload;
    }

    if (payload_length > kMaxPayloadSize) {
        return ProtocolError::payload_too_large;
    }

    return ProtocolError::none;
}

}  // 命名空间 aegisflow::net::protocol
