#pragma once

#include "aegisflow/base/array_view.hpp"
#include "aegisflow/net/protocol_contract.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <vector>

namespace aegisflow::net {

enum class CodecState : std::uint8_t {
    NeedHeader,
    NeedBody,
    FrameReady,
    Error,
};

using DecodedFrame = std::vector<std::uint8_t>;

struct CodecConsumeResult {
    CodecState state = CodecState::NeedHeader;
    protocol::ProtocolError error = protocol::ProtocolError::none;
    std::size_t bytes_consumed = 0;
    std::vector<DecodedFrame> frames;
};

class LengthPrefixedCodec final {
public:
    explicit LengthPrefixedCodec(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource()
    ) : payload_(resource) {}
    ~LengthPrefixedCodec() = default;

    LengthPrefixedCodec(const LengthPrefixedCodec&) = delete;
    LengthPrefixedCodec& operator=(const LengthPrefixedCodec&) = delete;
    LengthPrefixedCodec(LengthPrefixedCodec&&) = default;
    LengthPrefixedCodec& operator=(LengthPrefixedCodec&&) = default;

    // 输入是任意 TCP 字节片段；一次调用可以返回零帧或多帧。
    // FrameReady 表示本次调用产生了帧，nextState() 表示后续真实续读位置。
    [[nodiscard]] CodecConsumeResult consume(
        base::ArrayView<const std::uint8_t> input
    );

    [[nodiscard]] CodecState nextState() const noexcept;
    [[nodiscard]] std::size_t bufferedBytes() const noexcept;
    [[nodiscard]] std::uint32_t expectedPayloadBytes() const noexcept;

private:
    void enterError(protocol::ProtocolError error) noexcept;

    protocol::FrameHeader header_{};
    std::size_t header_bytes_ = 0;
    std::uint32_t expected_payload_bytes_ = 0;
    std::pmr::vector<std::uint8_t> payload_;
    CodecState next_state_ = CodecState::NeedHeader;
    protocol::ProtocolError error_ = protocol::ProtocolError::none;
};

}  // 命名空间 aegisflow::net
