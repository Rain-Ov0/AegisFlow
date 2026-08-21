#include "aegisflow/benchmark/login_frame_codec.hpp"

#include "aegisflow/net/protocol_contract.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace aegisflow::benchmark {

namespace protocol = aegisflow::net::protocol;

std::vector<std::uint8_t> encodeLoginRequestFrame(
    const aegisflow::login::LoginRequest& request
) {
    std::string payload;
    if (!request.SerializeToString(&payload) || payload.empty() ||
        payload.size() > protocol::kMaxPayloadSize) {
        throw std::runtime_error("benchmark 请求序列化失败");
    }

    const auto header = protocol::encodePayloadLength(
        static_cast<std::uint32_t>(payload.size())
    );
    std::vector<std::uint8_t> frame;
    frame.reserve(header.size() + payload.size());
    frame.insert(frame.end(), header.begin(), header.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

LoginResponseFrame decodeLoginResponseFrame(
    const base::ArrayView<const std::uint8_t> frame
) {
    LoginResponseFrame result;
    if (frame.size() < protocol::kFrameHeaderSize) {
        result.error = LoginFrameError::TruncatedFrame;
        return result;
    }

    protocol::FrameHeader header{};
    for (std::size_t index = 0; index < header.size(); ++index) {
        header[index] = frame[index];
    }
    const auto payload_size = protocol::decodePayloadLength(header);
    if (protocol::validatePayloadLength(payload_size) !=
        protocol::ProtocolError::none) {
        result.error = LoginFrameError::InvalidPayloadLength;
        return result;
    }

    const auto expected_size = protocol::kFrameHeaderSize +
                               static_cast<std::size_t>(payload_size);
    if (frame.size() < expected_size) {
        result.error = LoginFrameError::TruncatedFrame;
        return result;
    }
    if (frame.size() > expected_size) {
        result.error = LoginFrameError::TrailingBytes;
        return result;
    }
    if (payload_size > static_cast<std::uint32_t>(
                           std::numeric_limits<int>::max())) {
        result.error = LoginFrameError::InvalidPayloadLength;
        return result;
    }

    const auto payload = frame.subview(protocol::kFrameHeaderSize);
    if (!result.response.ParseFromArray(
            payload.data(), static_cast<int>(payload.size()))) {
        result.error = LoginFrameError::ProtobufParse;
    }
    return result;
}

}  // namespace aegisflow::benchmark
