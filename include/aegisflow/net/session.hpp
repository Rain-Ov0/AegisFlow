#pragma once

#include "aegisflow/base/array_view.hpp"
#include "aegisflow/net/connection_token.hpp"
#include "aegisflow/net/length_prefixed_codec.hpp"
#include "aegisflow/timer/timer_core.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <vector>

namespace aegisflow::net {

enum class SessionState : std::uint8_t {
    Reading,
    Processing,
    Writing,
    Closing,
    Closed,
};

enum class SessionStatus : std::uint8_t {
    Ok,
    Buffered,
    RequestReady,
    InvalidState,
    InvalidArgument,
    StaleToken,
    InputLimitExceeded,
    OutputLimitExceeded,
    ProtocolError,
    TruncatedFrame,
    WriteProgressExceeded,
    Closing,
    Closed,
};

enum class SessionCloseReason : std::uint8_t {
    None,
    LocalShutdown,
    ProtocolError,
    InputLimitExceeded,
    OutputLimitExceeded,
    PeerClosed,
    PeerTruncatedFrame,
    WriteInvariantViolation,
    IoError,
    BusinessQueueRejected,
    BusinessError,
    IdleTimeout,
    ReadTimeout,
    WriteTimeout,
    BusinessTimeout,
    ResourceExhausted,
};

enum class SessionInterest : std::uint8_t {
    None = 0,
    Read = 1U << 0U,
    Write = 1U << 1U,
    PeerReadClose = 1U << 2U,
};

[[nodiscard]] constexpr SessionInterest operator|(
    const SessionInterest left,
    const SessionInterest right
) noexcept {
    return static_cast<SessionInterest>(
        static_cast<std::uint8_t>(left) |
        static_cast<std::uint8_t>(right)
    );
}

[[nodiscard]] constexpr bool hasInterest(
    const SessionInterest interests,
    const SessionInterest interest
) noexcept {
    return (static_cast<std::uint8_t>(interests) &
            static_cast<std::uint8_t>(interest)) != 0;
}

enum class SessionTimerKind : std::uint8_t {
    Idle,
    Read,
    Write,
    Business,
};

struct SessionTimers {
    timer::TimerId idle;
    timer::TimerId read;
    timer::TimerId write;
    timer::TimerId business;
};

struct SessionConfig {
    std::size_t max_frame_payload_bytes = protocol::kMaxPayloadSize;
    std::size_t input_soft_watermark_bytes = 64U * 1024U;
    std::size_t max_input_buffer_bytes =
        protocol::kFrameHeaderSize + protocol::kMaxPayloadSize;
    std::size_t output_soft_watermark_bytes = 64U * 1024U;
    std::size_t max_output_buffer_bytes =
        protocol::kFrameHeaderSize + protocol::kMaxPayloadSize;

    [[nodiscard]] constexpr bool operator==(
        const SessionConfig& other
    ) const noexcept {
        return max_frame_payload_bytes == other.max_frame_payload_bytes &&
               input_soft_watermark_bytes ==
                   other.input_soft_watermark_bytes &&
               max_input_buffer_bytes == other.max_input_buffer_bytes &&
               output_soft_watermark_bytes ==
                   other.output_soft_watermark_bytes &&
               max_output_buffer_bytes == other.max_output_buffer_bytes;
    }
};

struct SessionResult {
    SessionStatus status = SessionStatus::Ok;
    std::optional<DecodedFrame> request;
    std::size_t bytes_accepted = 0;
    protocol::ProtocolError protocol_error = protocol::ProtocolError::none;
};

class ISessionConnection {
public:
    virtual ~ISessionConnection() = default;

    [[nodiscard]] virtual int fd() const noexcept = 0;
    virtual void close() noexcept = 0;
};

class Session final {
public:
    Session(
        ConnectionToken token,
        SessionConfig config,
        std::unique_ptr<ISessionConnection> connection,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource()
    );
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    [[nodiscard]] SessionResult onInput(
        base::ArrayView<const std::uint8_t> input
    );
    [[nodiscard]] SessionStatus queueResponse(
        ConnectionToken token,
        base::ArrayView<const std::uint8_t> response
    );
    [[nodiscard]] SessionStatus closeAfterWrite(
        SessionCloseReason reason
    ) noexcept;
    [[nodiscard]] SessionResult onBytesWritten(std::size_t bytes_written);
    [[nodiscard]] SessionStatus onPeerReadClosed() noexcept;

    // beginClose 只冻结状态，让 EventLoop 先执行 epoll DEL 和取消定时器。
    // finalizeClose 才调用连接句柄的 close，并且整个生命周期只调用一次。
    [[nodiscard]] SessionStatus beginClose(
        SessionCloseReason reason
    ) noexcept;
    [[nodiscard]] SessionStatus finalizeClose() noexcept;

    [[nodiscard]] timer::TimerId replaceTimer(
        SessionTimerKind kind,
        timer::TimerId timer_id
    ) noexcept;
    [[nodiscard]] SessionTimers releaseTimersForClose() noexcept;

    [[nodiscard]] const ConnectionToken& token() const noexcept;
    [[nodiscard]] bool matches(ConnectionToken token) const noexcept;
    [[nodiscard]] SessionState state() const noexcept;
    [[nodiscard]] SessionInterest interest() const noexcept;
    [[nodiscard]] SessionCloseReason closeReason() const noexcept;
    [[nodiscard]] std::size_t bufferedInputBytes() const noexcept;
    [[nodiscard]] base::ArrayView<const std::uint8_t> pendingOutput() const noexcept;

private:
    [[nodiscard]] static bool validConfig(
        const SessionConfig& config
    ) noexcept;
    [[nodiscard]] std::size_t pendingInputBytes() const noexcept;
    void appendPendingInput(base::ArrayView<const std::uint8_t> input);
    void clearConsumedPendingInput() noexcept;
    [[nodiscard]] SessionResult consumeReadingBytes(
        base::ArrayView<const std::uint8_t> input,
        bool store_remainder
    );
    [[nodiscard]] SessionResult consumePendingInput();
    [[nodiscard]] std::size_t bytesNeededByCodec() const noexcept;
    [[nodiscard]] bool inputAtOrAboveSoftWatermark() const noexcept;
    [[nodiscard]] std::size_t remainingOutputBytes() const noexcept;
    [[nodiscard]] bool outputAtOrAboveSoftWatermark() const noexcept;
    [[nodiscard]] bool readPausedByBackpressure() const noexcept;
    [[nodiscard]] timer::TimerId& timerSlot(SessionTimerKind kind) noexcept;
    [[nodiscard]] const timer::TimerId& timerSlot(
        SessionTimerKind kind
    ) const noexcept;

    ConnectionToken token_;
    SessionConfig config_;
    std::unique_ptr<ISessionConnection> connection_;
    LengthPrefixedCodec codec_;
    std::pmr::vector<std::uint8_t> pending_input_;
    std::size_t pending_input_offset_ = 0;
    std::pmr::vector<std::uint8_t> output_;
    std::size_t output_offset_ = 0;
    SessionTimers timers_;
    SessionState state_ = SessionState::Reading;
    SessionCloseReason close_reason_ = SessionCloseReason::None;
    bool peer_read_closed_ = false;
    bool close_after_write_ = false;
    bool connection_closed_ = false;
};

}  // 命名空间 aegisflow::net
