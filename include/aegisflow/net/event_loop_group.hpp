#pragma once

#include "aegisflow/net/event_loop.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace aegisflow::net {

enum class EventLoopGroupStatus : std::uint8_t {
    Ok,
    InvalidState,
    StartFailed,
    SelfJoin,
    DrainDeadlineExceeded,
};

struct EventLoopGroupConfig {
    std::size_t loop_count = 1;
    std::size_t max_connections = 65536;
    std::size_t max_connections_per_loop = 65536;
    std::size_t connection_queue_capacity = 1024;
    EventLoopConfig event_loop;

    bool operator==(const EventLoopGroupConfig& other) const {
        return loop_count == other.loop_count &&
               max_connections == other.max_connections &&
               max_connections_per_loop ==
                   other.max_connections_per_loop &&
               connection_queue_capacity ==
                   other.connection_queue_capacity &&
               event_loop == other.event_loop;
    }
};

class EventLoopGroup final {
public:
    EventLoopGroup(
        EventLoopGroupConfig config,
        runtime::BoundedWorkerPool& worker_pool,
        std::shared_ptr<IFrameBusinessHandler> business_handler,
        timer::ITimerScheduler& timer_scheduler
    );
    ~EventLoopGroup();

    EventLoopGroup(const EventLoopGroup&) = delete;
    EventLoopGroup& operator=(const EventLoopGroup&) = delete;
    EventLoopGroup(EventLoopGroup&&) = delete;
    EventLoopGroup& operator=(EventLoopGroup&&) = delete;

    [[nodiscard]] EventLoopGroupStatus start() noexcept;
    [[nodiscard]] EventLoopGroupStatus beginDrain() noexcept;
    [[nodiscard]] EventLoopGroupStatus drainUntil(
        std::chrono::steady_clock::time_point deadline
    ) noexcept;
    void stop() noexcept;
    [[nodiscard]] EventLoopGroupStatus join() noexcept;
    void tryDispatch(OwnedSocket socket) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // 命名空间 aegisflow::net
