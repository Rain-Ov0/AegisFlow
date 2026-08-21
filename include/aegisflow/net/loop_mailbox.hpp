#pragma once

#include "aegisflow/net/completion_router.hpp"
#include "aegisflow/net/owned_socket.hpp"
#include "aegisflow/net/protocol_contract.hpp"
#include "aegisflow/timer/timer_core.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace aegisflow::net {

class LoopMailbox final {
public:
    explicit LoopMailbox(
        std::size_t capacity,
        std::size_t byte_capacity = 16U * 1024U * 1024U,
        std::size_t max_completion_bytes =
            protocol::kFrameHeaderSize + protocol::kMaxPayloadSize
    );
    ~LoopMailbox();

    LoopMailbox(const LoopMailbox&) = delete;
    LoopMailbox& operator=(const LoopMailbox&) = delete;
    LoopMailbox(LoopMailbox&&) = delete;
    LoopMailbox& operator=(LoopMailbox&&) = delete;

    [[nodiscard]] bool tryPost(
        BusinessCompletion completion
    ) noexcept;
    [[nodiscard]] std::optional<BusinessCompletion> tryPop() noexcept;
    [[nodiscard]] bool drainWakeSignal() noexcept;
    void close() noexcept;

    [[nodiscard]] int wakeFd() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class ConnectionMailbox final {
public:
    explicit ConnectionMailbox(std::size_t capacity);
    ~ConnectionMailbox();

    ConnectionMailbox(const ConnectionMailbox&) = delete;
    ConnectionMailbox& operator=(const ConnectionMailbox&) = delete;

    [[nodiscard]] bool tryPost(
        OwnedSocket socket
    ) noexcept;
    [[nodiscard]] std::optional<OwnedSocket> tryPop() noexcept;
    [[nodiscard]] bool drainWakeSignal() noexcept;
    [[nodiscard]] std::size_t close() noexcept;
    [[nodiscard]] int wakeFd() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class TimeoutMailbox final : public timer::ITimerSink {
public:
    explicit TimeoutMailbox(std::size_t capacity);
    ~TimeoutMailbox();

    TimeoutMailbox(const TimeoutMailbox&) = delete;
    TimeoutMailbox& operator=(const TimeoutMailbox&) = delete;

    [[nodiscard]] bool tryPost(timer::TimerEvent event) noexcept override;
    [[nodiscard]] std::optional<timer::TimerEvent> tryPop() noexcept;
    [[nodiscard]] bool drainWakeSignal() noexcept;
    [[nodiscard]] bool takeRescanRequest() noexcept;
    void close() noexcept;
    [[nodiscard]] int wakeFd() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // 命名空间 aegisflow::net
