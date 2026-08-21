#pragma once

#include "aegisflow/net/completion_router.hpp"
#include "aegisflow/net/loop_mailbox.hpp"
#include "aegisflow/net/owned_socket.hpp"
#include "aegisflow/net/session.hpp"
#include "aegisflow/runtime/bounded_worker_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace aegisflow::net {

class EventLoopDrainControl final {
public:
    void requestDrain() noexcept {
        drain_requested_.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool drainRequested() const noexcept {
        return drain_requested_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> drain_requested_{false};
};

enum class EventLoopStatus : std::uint8_t {
    Ok,
    InvalidArgument,
    WrongThread,
    AlreadyRegistered,
    GenerationExhausted,
    ConnectionLimit,
    TimerScheduleFailed,
    SystemCallFailed,
    Stopped,
};

struct SessionDeadlineConfig {
    bool enabled = false;
    std::chrono::milliseconds idle_timeout{0};
    std::chrono::milliseconds read_timeout{0};
    std::chrono::milliseconds write_timeout{0};
    std::chrono::milliseconds business_timeout{0};

    [[nodiscard]] constexpr bool operator==(
        const SessionDeadlineConfig& other
    ) const noexcept {
        return enabled == other.enabled &&
               idle_timeout == other.idle_timeout &&
               read_timeout == other.read_timeout &&
               write_timeout == other.write_timeout &&
               business_timeout == other.business_timeout;
    }
};

struct EventLoopConfig {
    std::uint32_t loop_id = 0;
    std::size_t max_connections = 65536;
    std::size_t max_events = 64;
    std::size_t read_scratch_bytes = 4096;
    std::size_t completion_capacity = 1024;
    std::size_t completion_byte_capacity = 16U * 1024U * 1024U;
    std::size_t max_completion_bytes =
        protocol::kFrameHeaderSize + protocol::kMaxPayloadSize;
    std::vector<std::uint8_t> overload_response_frame;
    std::vector<std::uint8_t> business_timeout_response_frame;
    SessionDeadlineConfig deadlines;
    SessionConfig session;

    bool operator==(const EventLoopConfig& other) const {
        return loop_id == other.loop_id &&
               max_connections == other.max_connections &&
               max_events == other.max_events &&
               read_scratch_bytes == other.read_scratch_bytes &&
               completion_capacity == other.completion_capacity &&
               completion_byte_capacity == other.completion_byte_capacity &&
               max_completion_bytes == other.max_completion_bytes &&
               overload_response_frame == other.overload_response_frame &&
               business_timeout_response_frame ==
                   other.business_timeout_response_frame &&
               deadlines == other.deadlines &&
               session == other.session;
    }
};

class EventLoop final {
public:
    EventLoop(
        EventLoopConfig config,
        runtime::BoundedWorkerPool& worker_pool,
        std::shared_ptr<IFrameBusinessHandler> business_handler,
        timer::ITimerScheduler& timer_scheduler,
        std::shared_ptr<ConnectionMailbox> connection_mailbox = nullptr,
        std::function<void()> release_connection = {},
        std::shared_ptr<CompletionRouter> completion_router = nullptr,
        std::shared_ptr<EventLoopDrainControl> drain_control = nullptr
    );
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;
    EventLoop(EventLoop&&) = delete;
    EventLoop& operator=(EventLoop&&) = delete;

    [[nodiscard]] EventLoopStatus adopt(OwnedSocket socket) noexcept;
    [[nodiscard]] EventLoopStatus pollOnce(int timeout_ms) noexcept;
    [[nodiscard]] EventLoopStatus shutdown() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // 命名空间 aegisflow::net
