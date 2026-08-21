#include "aegisflow/net/session.hpp"

#include "aegisflow/base/array_view.hpp"
#include "tests/support/test_harness.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace {

using aegisflow::test::require;

[[nodiscard]] aegisflow::net::ConnectionToken connectionToken(
    const std::uint32_t loop_id,
    const int fd,
    const std::uint64_t generation
) {
    aegisflow::net::ConnectionToken token;
    token.loop_id = loop_id;
    token.fd = fd;
    token.generation = generation;
    return token;
}

std::vector<std::uint8_t> frame(const std::string_view payload) {
    const auto header = aegisflow::net::protocol::encodePayloadLength(
        static_cast<std::uint32_t>(payload.size())
    );
    std::vector<std::uint8_t> bytes(header.begin(), header.end());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

class TestConnection final : public aegisflow::net::ISessionConnection {
public:
    explicit TestConnection(const int socket_fd) : socket_fd_(socket_fd) {}

    [[nodiscard]] int fd() const noexcept override {
        return socket_fd_;
    }

    void close() noexcept override {
        closed_ = true;
    }

    [[nodiscard]] bool closed() const noexcept {
        return closed_;
    }

private:
    int socket_fd_ = -1;
    bool closed_ = false;
};

void staleGenerationCannotWriteResponse() {
    using aegisflow::net::ConnectionToken;
    using aegisflow::net::Session;
    using aegisflow::net::SessionStatus;

    const ConnectionToken token = connectionToken(2, 77, 9);
    Session session(token, {}, std::make_unique<TestConnection>(token.fd));
    const auto request_frame = frame("request");
    const auto input = session.onInput(request_frame);
    require(input.status == SessionStatus::RequestReady, "Session 必须进入业务处理状态");
    require(
        input.request.has_value() &&
            std::string(input.request->begin(), input.request->end()) == "request",
        "Session 必须保留完整请求载荷"
    );

    const std::array<std::uint8_t, 1> response = {1};
    const ConnectionToken stale = connectionToken(
        token.loop_id,
        token.fd,
        token.generation - 1
    );
    require(
        session.queueResponse(stale, response) == SessionStatus::StaleToken,
        "旧 generation 的完成结果必须被拒绝"
    );
    require(
        session.queueResponse(token, response) == SessionStatus::Ok,
        "完整匹配的 token 必须允许写回"
    );
}

void incompleteFrameAtPeerCloseIsReported() {
    using aegisflow::net::ConnectionToken;
    using aegisflow::net::Session;
    using aegisflow::net::SessionCloseReason;
    using aegisflow::net::SessionStatus;

    const ConnectionToken token = connectionToken(1, 78, 1);
    Session session(token, {}, std::make_unique<TestConnection>(token.fd));
    const auto request_frame = frame("request");
    const auto partial =
        aegisflow::base::ArrayView<const std::uint8_t>(request_frame).first(3);
    require(
        session.onInput(partial).status == SessionStatus::Buffered,
        "不完整帧必须缓冲"
    );
    require(
        session.onPeerReadClosed() == SessionStatus::TruncatedFrame &&
            session.closeReason() == SessionCloseReason::PeerTruncatedFrame,
        "对端在半帧时关闭必须报告截断帧"
    );
}

void processingStateBuffersOneFollowingRequest() {
    using aegisflow::net::ConnectionToken;
    using aegisflow::net::Session;
    using aegisflow::net::SessionStatus;

    const ConnectionToken token = connectionToken(1, 79, 1);
    Session session(token, {}, std::make_unique<TestConnection>(token.fd));
    const auto first_frame = frame("first");
    require(
        session.onInput(first_frame).status == SessionStatus::RequestReady,
        "首个请求必须进入处理状态"
    );
    const auto second_frame = frame("second");
    require(
        session.onInput(second_frame).status == SessionStatus::Buffered,
        "单请求在途时后续字节必须有界缓冲"
    );

    const std::array<std::uint8_t, 1> response = {1};
    require(session.queueResponse(token, response) == SessionStatus::Ok, "首个响应必须入队");
    const auto completed = session.onBytesWritten(response.size());
    require(
        completed.status == SessionStatus::RequestReady &&
            completed.request.has_value() &&
            std::string(completed.request->begin(), completed.request->end()) ==
                "second",
        "首个响应写完后必须按序解码缓冲请求"
    );
}

}  // namespace

int main() {
    return aegisflow::test::runModule(
        "session",
        {
            {"generation token", staleGenerationCannotWriteResponse},
            {"截断帧关闭", incompleteFrameAtPeerCloseIsReported},
            {"单请求在途", processingStateBuffersOneFollowingRequest},
        }
    );
}
