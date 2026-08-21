#include "aegisflow/net/length_prefixed_codec.hpp"

#include "aegisflow/base/array_view.hpp"
#include "tests/support/test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using aegisflow::test::require;

std::vector<std::uint8_t> frame(const std::string_view payload) {
    const auto header = aegisflow::net::protocol::encodePayloadLength(
        static_cast<std::uint32_t>(payload.size())
    );
    std::vector<std::uint8_t> bytes(header.begin(), header.end());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

void splitHeaderAndPayloadAreBuffered() {
    using aegisflow::net::CodecState;
    using aegisflow::net::LengthPrefixedCodec;

    const auto bytes = frame("abc");
    LengthPrefixedCodec codec;

    auto result = codec.consume(
        aegisflow::base::ArrayView<const std::uint8_t>(bytes).first(2));
    require(result.state == CodecState::NeedHeader, "半个帧头不得产生消息");
    require(codec.bufferedBytes() == 2, "帧头缓冲字节数不一致");

    result = codec.consume(
        aegisflow::base::ArrayView<const std::uint8_t>(bytes).subview(2, 3));
    require(result.state == CodecState::NeedBody, "半个帧体不得产生消息");
    require(codec.expectedPayloadBytes() == 3, "帧头声明长度应保留");

    result = codec.consume(
        aegisflow::base::ArrayView<const std::uint8_t>(bytes).subview(5));
    require(result.frames.size() == 1, "完整半包必须还原一帧");
    require(
        std::string(result.frames.front().begin(), result.frames.front().end()) ==
            "abc",
        "半包还原的载荷不一致"
    );
    require(codec.nextState() == CodecState::NeedHeader, "完整帧后应继续读帧头");
}

void joinedFramesAreDecodedInOrder() {
    using aegisflow::net::LengthPrefixedCodec;

    auto joined = frame("first");
    const auto second = frame("second");
    joined.insert(joined.end(), second.begin(), second.end());

    LengthPrefixedCodec codec;
    const auto result = codec.consume(joined);
    require(result.frames.size() == 2, "粘包必须一次产生两帧");
    require(
        std::string(result.frames[0].begin(), result.frames[0].end()) == "first" &&
            std::string(result.frames[1].begin(), result.frames[1].end()) == "second",
        "粘包的帧顺序或载荷发生变化"
    );
}

void invalidLengthsEnterStickyErrorState() {
    using aegisflow::net::CodecState;
    using aegisflow::net::LengthPrefixedCodec;
    using aegisflow::net::protocol::ProtocolError;

    LengthPrefixedCodec empty_codec;
    const auto empty = aegisflow::net::protocol::encodePayloadLength(0);
    auto result = empty_codec.consume(empty);
    require(result.state == CodecState::Error, "空载荷帧必须进入错误状态");
    require(result.error == ProtocolError::empty_payload, "空载荷错误类型不一致");

    LengthPrefixedCodec oversized_codec;
    const auto oversized = aegisflow::net::protocol::encodePayloadLength(
        aegisflow::net::protocol::kMaxPayloadSize + 1
    );
    result = oversized_codec.consume(oversized);
    require(result.state == CodecState::Error, "超限帧必须进入错误状态");
    require(
        result.error == ProtocolError::payload_too_large,
        "超限帧必须保留协议错误语义"
    );
    const auto ignored = frame("ignored");
    require(
        oversized_codec.consume(ignored).state == CodecState::Error,
        "Codec 错误状态不得隐式恢复"
    );
}

}  // namespace

int main() {
    return aegisflow::test::runModule(
        "length_prefixed_codec",
        {
            {"半包缓冲", splitHeaderAndPayloadAreBuffered},
            {"粘包顺序", joinedFramesAreDecodedInOrder},
            {"非法长度", invalidLengthsEnterStickyErrorState},
        }
    );
}
