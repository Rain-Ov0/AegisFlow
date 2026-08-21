#include "aegisflow/net/length_prefixed_codec.hpp"

#include <algorithm>

namespace aegisflow::net {

CodecConsumeResult LengthPrefixedCodec::consume(
    const base::ArrayView<const std::uint8_t> input
) {
    CodecConsumeResult result;

    if (next_state_ == CodecState::Error) {
        result.state = CodecState::Error;
        result.error = error_;
        return result;
    }

    std::size_t offset = 0;
    while (offset < input.size()) {
        if (next_state_ == CodecState::NeedHeader) {
            const auto header_remaining =
                protocol::kFrameHeaderSize - header_bytes_;
            const auto copy_count = std::min(
                header_remaining,
                input.size() - offset
            );

            std::copy_n(
                input.begin() + static_cast<std::ptrdiff_t>(offset),
                copy_count,
                header_.begin() + static_cast<std::ptrdiff_t>(header_bytes_)
            );
            header_bytes_ += copy_count;
            offset += copy_count;

            if (header_bytes_ < protocol::kFrameHeaderSize) {
                continue;
            }

            const auto declared_length = protocol::decodePayloadLength(header_);
            const auto error = protocol::validatePayloadLength(declared_length);
            header_bytes_ = 0;

            if (error != protocol::ProtocolError::none) {
                enterError(error);
                break;
            }

            expected_payload_bytes_ = declared_length;
            next_state_ = CodecState::NeedBody;
        }

        if (next_state_ == CodecState::NeedBody && offset < input.size()) {
            const auto payload_remaining =
                static_cast<std::size_t>(expected_payload_bytes_) -
                payload_.size();
            const auto copy_count = std::min(
                payload_remaining,
                input.size() - offset
            );

            payload_.insert(
                payload_.end(),
                input.begin() + static_cast<std::ptrdiff_t>(offset),
                input.begin() + static_cast<std::ptrdiff_t>(offset + copy_count)
            );
            offset += copy_count;

            if (payload_.size() == expected_payload_bytes_) {
                // 帧跨线程使用普通 vector；Codec 的 PMR 容量留给所属 loop 复用。
                result.frames.emplace_back(payload_.begin(), payload_.end());
                payload_.clear();
                expected_payload_bytes_ = 0;
                next_state_ = CodecState::NeedHeader;
            }
        }
    }

    result.bytes_consumed = offset;
    result.error = error_;

    if (next_state_ == CodecState::Error) {
        result.state = CodecState::Error;
    } else if (!result.frames.empty()) {
        result.state = CodecState::FrameReady;
    } else {
        result.state = next_state_;
    }

    return result;
}

CodecState LengthPrefixedCodec::nextState() const noexcept {
    return next_state_;
}

std::size_t LengthPrefixedCodec::bufferedBytes() const noexcept {
    return header_bytes_ + payload_.size();
}

std::uint32_t LengthPrefixedCodec::expectedPayloadBytes() const noexcept {
    return expected_payload_bytes_;
}

void LengthPrefixedCodec::enterError(
    const protocol::ProtocolError error
) noexcept {
    next_state_ = CodecState::Error;
    error_ = error;
    header_bytes_ = 0;
    expected_payload_bytes_ = 0;
    payload_.clear();
}

}  // 命名空间 aegisflow::net
