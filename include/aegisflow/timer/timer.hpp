#pragma once

#include "aegisflow/timer/timer_core.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace aegisflow::timer {

enum class TimerState : std::uint8_t {
    Constructed,
    Initialized,
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed,
};

struct TimerConfig {
    std::size_t command_capacity = 4096;
    std::size_t timer_capacity = 65536;

    [[nodiscard]] constexpr bool operator==(
        const TimerConfig& other
    ) const noexcept {
        return command_capacity == other.command_capacity &&
               timer_capacity == other.timer_capacity;
    }
};

class Timer final : public ITimerScheduler {
public:
    static Timer& instance();

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    Timer(Timer&&) = delete;
    Timer& operator=(Timer&&) = delete;

    [[nodiscard]] TimerStatus init(const TimerConfig& config) noexcept;
    [[nodiscard]] TimerStatus start() noexcept;
    void stop() noexcept;
    [[nodiscard]] TimerStatus join() noexcept;

    [[nodiscard]] TimerScheduleResult scheduleAt(
        SteadyTime deadline,
        std::weak_ptr<ITimerSink> sink,
        TimerEvent event
    ) noexcept override;
    [[nodiscard]] TimerStatus cancel(TimerId id) noexcept override;

private:
    Timer() noexcept = default;
    ~Timer();

    struct WaitResult {
        TimerStatus status = TimerStatus::Ok;
        bool interrupted = false;
        bool command_ready = false;
        bool timer_ready = false;
    };

    [[nodiscard]] static bool validConfig(
        const TimerConfig& config
    ) noexcept;
    [[nodiscard]] TimerStatus openFds() noexcept;
    void closeFds() noexcept;
    [[nodiscard]] TimerStatus notifyWorker() noexcept;
    [[nodiscard]] WaitResult waitForEvent() noexcept;
    [[nodiscard]] TimerStatus drainFd(int fd) noexcept;
    [[nodiscard]] TimerStatus arm(
        std::optional<SteadyTime> deadline
    ) noexcept;
    void workerLoop() noexcept;
    void setStateLocked(TimerState state) noexcept;

    TimerCore scheduler_;
    mutable std::mutex mutex_;
    mutable std::mutex fd_mutex_;
    std::condition_variable state_changed_;
    std::optional<TimerConfig> config_;
    std::thread worker_;
    std::atomic<TimerState> state_{TimerState::Constructed};
    int epoll_fd_ = -1;
    int event_fd_ = -1;
    int timer_fd_ = -1;
    TimerStatus start_status_ = TimerStatus::InternalError;
    TimerStatus join_status_ = TimerStatus::Ok;
    bool join_in_progress_ = false;
};

}  // 命名空间 aegisflow::timer
