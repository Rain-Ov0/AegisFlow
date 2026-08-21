#include "aegisflow/net/session.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace aegisflow::net {

namespace {

[[nodiscard]] SessionResult resultWithStatus(
    const SessionStatus status
) noexcept {
    SessionResult result;
    result.status = status;
    return result;
}

}  // 命名空间

Session::Session(
    const ConnectionToken token,
    const SessionConfig config,
    std::unique_ptr<ISessionConnection> connection,
    std::pmr::memory_resource* resource
) : token_(token),
    config_(config),
    connection_(std::move(connection)),
    codec_(resource),
    pending_input_(resource),
    output_(resource) {
    if (!token_.valid() || !validConfig(config_) || connection_ == nullptr ||
        connection_->fd() != token_.fd) {
        // 构造失败时 Session 析构不会执行，因此在抛出前归还已接收的 fd。
        if (connection_ != nullptr) {
            connection_->close();
            connection_closed_ = true;
        }
        throw std::invalid_argument("Session 所有权或容量配置无效");
    }
}

Session::~Session() {
    if (!connection_closed_ && connection_ != nullptr) {
        connection_->close();
        connection_closed_ = true;
    }
}

bool Session::validConfig(const SessionConfig& config) noexcept {
    return config.max_frame_payload_bytes > 0 &&
           config.max_frame_payload_bytes <= protocol::kMaxPayloadSize &&
           config.input_soft_watermark_bytes > 0 &&
           config.input_soft_watermark_bytes <=
               config.max_input_buffer_bytes &&
           config.max_input_buffer_bytes >=
               config.max_frame_payload_bytes &&
           config.output_soft_watermark_bytes > 0 &&
           config.output_soft_watermark_bytes <=
               config.max_output_buffer_bytes;
}

SessionResult Session::onInput(
    const base::ArrayView<const std::uint8_t> input
) {
    if (state_ == SessionState::Closing) {
        return resultWithStatus(SessionStatus::Closing);
    }
    if (state_ == SessionState::Closed) {
        return resultWithStatus(SessionStatus::Closed);
    }
    if (input.empty()) {
        return resultWithStatus(SessionStatus::Buffered);
    }

    if (state_ != SessionState::Reading) {
        // 单连接只允许一个业务请求在途；后续字节有界缓存，待响应写完再解码。
        if (input.size() >
            config_.max_input_buffer_bytes - bufferedInputBytes()) {
            (void)beginClose(SessionCloseReason::InputLimitExceeded);
            return resultWithStatus(SessionStatus::InputLimitExceeded);
        }
        appendPendingInput(input);
        auto result = resultWithStatus(SessionStatus::Buffered);
        result.bytes_accepted = input.size();
        return result;
    }

    return consumeReadingBytes(input, true);
}

SessionResult Session::consumeReadingBytes(
    const base::ArrayView<const std::uint8_t> input,
    const bool store_remainder
) {
    auto session_result = resultWithStatus(SessionStatus::Buffered);
    std::size_t offset = 0;

    while (offset < input.size() && state_ == SessionState::Reading) {
        const auto consume_count = std::min(
            bytesNeededByCodec(),
            input.size() - offset
        );
        if (store_remainder &&
            consume_count >
                config_.max_input_buffer_bytes - bufferedInputBytes()) {
            (void)beginClose(SessionCloseReason::InputLimitExceeded);
            session_result.status = SessionStatus::InputLimitExceeded;
            session_result.bytes_accepted = offset;
            return session_result;
        }
        const auto codec_result = codec_.consume(
            input.subview(offset, consume_count)
        );
        offset += codec_result.bytes_consumed;

        if (codec_result.state == CodecState::Error) {
            (void)beginClose(SessionCloseReason::ProtocolError);
            session_result.status = SessionStatus::ProtocolError;
            session_result.protocol_error = codec_result.error;
            session_result.bytes_accepted = offset;
            return session_result;
        }

        if (codec_.nextState() == CodecState::NeedBody &&
            codec_.expectedPayloadBytes() >
                config_.max_frame_payload_bytes) {
            (void)beginClose(SessionCloseReason::InputLimitExceeded);
            session_result.status = SessionStatus::InputLimitExceeded;
            session_result.bytes_accepted = offset;
            return session_result;
        }

        if (!codec_result.frames.empty()) {
            if (codec_result.frames.size() != 1) {
                // 同一次输入解出多帧会破坏单请求在途约束，因此明确关闭。
                (void)beginClose(SessionCloseReason::ProtocolError);
                session_result.status = SessionStatus::ProtocolError;
                session_result.protocol_error =
                    protocol::ProtocolError::multiple_requests;
                session_result.bytes_accepted = offset;
                return session_result;
            }

            session_result.request = std::move(codec_result.frames.front());
            session_result.status = SessionStatus::RequestReady;
            state_ = SessionState::Processing;
        }
    }

    if (store_remainder && offset < input.size()) {
        if (input.size() - offset >
            config_.max_input_buffer_bytes - bufferedInputBytes()) {
            (void)beginClose(SessionCloseReason::InputLimitExceeded);
            session_result.status = SessionStatus::InputLimitExceeded;
            session_result.request.reset();
            session_result.bytes_accepted = offset;
            return session_result;
        }
        appendPendingInput(input.subview(offset));
        offset = input.size();
    }
    session_result.bytes_accepted = offset;
    return session_result;
}

SessionResult Session::consumePendingInput() {
    if (state_ != SessionState::Reading || pendingInputBytes() == 0) {
        return resultWithStatus(SessionStatus::Buffered);
    }

    const auto pending = base::ArrayView<const std::uint8_t>(pending_input_).subview(
        pending_input_offset_
    );
    auto result = consumeReadingBytes(pending, false);
    pending_input_offset_ += result.bytes_accepted;
    clearConsumedPendingInput();
    return result;
}

SessionStatus Session::queueResponse(
    const ConnectionToken token,
    const base::ArrayView<const std::uint8_t> response
) {
    if (!matches(token)) {
        // fd 可被操作系统复用，generation 不匹配的旧 completion 不得写入新连接。
        return SessionStatus::StaleToken;
    }
    if (state_ == SessionState::Closing) {
        return SessionStatus::Closing;
    }
    if (state_ == SessionState::Closed) {
        return SessionStatus::Closed;
    }
    if (state_ != SessionState::Processing) {
        return SessionStatus::InvalidState;
    }
    if (response.empty()) {
        return SessionStatus::InvalidArgument;
    }
    if (response.size() > config_.max_output_buffer_bytes) {
        (void)beginClose(SessionCloseReason::OutputLimitExceeded);
        return SessionStatus::OutputLimitExceeded;
    }

    output_.assign(response.begin(), response.end());
    output_offset_ = 0;
    state_ = SessionState::Writing;
    return SessionStatus::Ok;
}

SessionResult Session::onBytesWritten(const std::size_t bytes_written) {
    if (state_ == SessionState::Closing) {
        return resultWithStatus(SessionStatus::Closing);
    }
    if (state_ == SessionState::Closed) {
        return resultWithStatus(SessionStatus::Closed);
    }
    if (state_ != SessionState::Writing) {
        return resultWithStatus(SessionStatus::InvalidState);
    }
    if (bytes_written > remainingOutputBytes()) {
        (void)beginClose(SessionCloseReason::WriteInvariantViolation);
        return resultWithStatus(SessionStatus::WriteProgressExceeded);
    }

    output_offset_ += bytes_written;
    // 短写只推进 offset，未写完部分继续由 EPOLLOUT 驱动。
    if (remainingOutputBytes() != 0) {
        return resultWithStatus(SessionStatus::Ok);
    }

    output_.clear();
    output_offset_ = 0;
    if (close_after_write_ || peer_read_closed_) {
        (void)beginClose(
            close_after_write_
                ? close_reason_
                : SessionCloseReason::PeerClosed
        );
        return resultWithStatus(SessionStatus::Closing);
    }

    state_ = SessionState::Reading;
    auto result = consumePendingInput();
    if (result.status == SessionStatus::Buffered) {
        result.status = SessionStatus::Ok;
    }
    return result;
}

SessionStatus Session::closeAfterWrite(
    const SessionCloseReason reason
) noexcept {
    if (state_ == SessionState::Closing) {
        return SessionStatus::Closing;
    }
    if (state_ == SessionState::Closed) {
        return SessionStatus::Closed;
    }
    if (state_ != SessionState::Writing ||
        reason == SessionCloseReason::None) {
        return SessionStatus::InvalidState;
    }
    close_reason_ = reason;
    close_after_write_ = true;
    return SessionStatus::Ok;
}

SessionStatus Session::onPeerReadClosed() noexcept {
    if (state_ == SessionState::Closing) {
        return SessionStatus::Closing;
    }
    if (state_ == SessionState::Closed) {
        return SessionStatus::Closed;
    }

    peer_read_closed_ = true;
    if (state_ == SessionState::Processing ||
        state_ == SessionState::Writing) {
        close_reason_ = SessionCloseReason::PeerClosed;
        return SessionStatus::Ok;
    }

    if (bufferedInputBytes() != 0) {
        (void)beginClose(SessionCloseReason::PeerTruncatedFrame);
        return SessionStatus::TruncatedFrame;
    }

    (void)beginClose(SessionCloseReason::PeerClosed);
    return SessionStatus::Closing;
}

SessionStatus Session::beginClose(
    const SessionCloseReason reason
) noexcept {
    if (state_ == SessionState::Closed) {
        return SessionStatus::Closed;
    }
    if (state_ == SessionState::Closing) {
        return SessionStatus::Closing;
    }
    if (reason == SessionCloseReason::None) {
        return SessionStatus::InvalidArgument;
    }

    state_ = SessionState::Closing;
    close_reason_ = reason;
    return SessionStatus::Closing;
}

SessionStatus Session::finalizeClose() noexcept {
    if (state_ == SessionState::Closed) {
        return SessionStatus::Closed;
    }
    if (state_ != SessionState::Closing) {
        (void)beginClose(SessionCloseReason::LocalShutdown);
    }
    if (!connection_closed_) {
        connection_->close();
        connection_closed_ = true;
    }
    state_ = SessionState::Closed;
    return SessionStatus::Ok;
}

timer::TimerId Session::replaceTimer(
    const SessionTimerKind kind,
    const timer::TimerId timer_id
) noexcept {
    auto& slot = timerSlot(kind);
    const auto previous = slot;
    slot = timer_id;
    return previous;
}

SessionTimers Session::releaseTimersForClose() noexcept {
    const auto released = timers_;
    timers_ = {};
    return released;
}

const ConnectionToken& Session::token() const noexcept {
    return token_;
}

bool Session::matches(const ConnectionToken token) const noexcept {
    return token_ == token;
}

SessionState Session::state() const noexcept {
    return state_;
}

SessionInterest Session::interest() const noexcept {
    if (state_ == SessionState::Closing ||
        state_ == SessionState::Closed) {
        return SessionInterest::None;
    }

    auto interests = SessionInterest::None;
    if (state_ == SessionState::Writing) {
        interests = interests | SessionInterest::Write;
    }
    if (peer_read_closed_) {
        return interests;
    }

    interests = interests | SessionInterest::PeerReadClose;
    if (state_ == SessionState::Reading ||
        !readPausedByBackpressure()) {
        interests = interests | SessionInterest::Read;
    }
    return interests;
}

SessionCloseReason Session::closeReason() const noexcept {
    return close_reason_;
}

std::size_t Session::pendingInputBytes() const noexcept {
    return pending_input_.size() - pending_input_offset_;
}

std::size_t Session::bufferedInputBytes() const noexcept {
    return codec_.bufferedBytes() + pendingInputBytes();
}

bool Session::inputAtOrAboveSoftWatermark() const noexcept {
    return bufferedInputBytes() >= config_.input_soft_watermark_bytes;
}

std::size_t Session::remainingOutputBytes() const noexcept {
    return output_.size() - output_offset_;
}

base::ArrayView<const std::uint8_t> Session::pendingOutput() const noexcept {
    return base::ArrayView<const std::uint8_t>(output_).subview(output_offset_);
}

bool Session::outputAtOrAboveSoftWatermark() const noexcept {
    return remainingOutputBytes() >= config_.output_soft_watermark_bytes;
}

bool Session::readPausedByBackpressure() const noexcept {
    if (peer_read_closed_ ||
        (state_ != SessionState::Processing &&
         state_ != SessionState::Writing)) {
        return false;
    }
    // 软水位只暂停 EPOLLIN，硬上限仍在追加输入/输出时强制校验。
    return inputAtOrAboveSoftWatermark() ||
           outputAtOrAboveSoftWatermark();
}

void Session::appendPendingInput(
    const base::ArrayView<const std::uint8_t> input
) {
    if (input.empty()) {
        return;
    }
    if (pending_input_offset_ != 0) {
        pending_input_.erase(
            pending_input_.begin(),
            pending_input_.begin() +
                static_cast<std::ptrdiff_t>(pending_input_offset_)
        );
        pending_input_offset_ = 0;
    }
    pending_input_.insert(pending_input_.end(), input.begin(), input.end());
}

void Session::clearConsumedPendingInput() noexcept {
    if (pending_input_offset_ == pending_input_.size()) {
        pending_input_.clear();
        pending_input_offset_ = 0;
    }
}

std::size_t Session::bytesNeededByCodec() const noexcept {
    if (codec_.nextState() == CodecState::NeedHeader) {
        return protocol::kFrameHeaderSize - codec_.bufferedBytes();
    }
    if (codec_.nextState() == CodecState::NeedBody) {
        return static_cast<std::size_t>(codec_.expectedPayloadBytes()) -
               codec_.bufferedBytes();
    }
    return 0;
}

timer::TimerId& Session::timerSlot(const SessionTimerKind kind) noexcept {
    switch (kind) {
        case SessionTimerKind::Idle:
            return timers_.idle;
        case SessionTimerKind::Read:
            return timers_.read;
        case SessionTimerKind::Write:
            return timers_.write;
        case SessionTimerKind::Business:
            return timers_.business;
    }
    return timers_.idle;
}

const timer::TimerId& Session::timerSlot(
    const SessionTimerKind kind
) const noexcept {
    switch (kind) {
        case SessionTimerKind::Idle:
            return timers_.idle;
        case SessionTimerKind::Read:
            return timers_.read;
        case SessionTimerKind::Write:
            return timers_.write;
        case SessionTimerKind::Business:
            return timers_.business;
    }
    return timers_.idle;
}

}  // 命名空间 aegisflow::net
