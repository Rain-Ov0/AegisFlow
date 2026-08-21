#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <unordered_map>
#include <vector>

namespace aegisflow::timer {

using SteadyClock = std::chrono::steady_clock;
using SteadyTime = SteadyClock::time_point;

struct TimerId {
    std::uint64_t value = 0;
    std::uint64_t generation = 0;

    [[nodiscard]] bool valid() const noexcept {
        return value != 0 && generation != 0;
    }

    [[nodiscard]] constexpr bool operator==(
        const TimerId& other
    ) const noexcept {
        return value == other.value && generation == other.generation;
    }
};

enum class TimerEventKind : std::uint8_t {
    IdleTimeout,
    ReadTimeout,
    WriteTimeout,
    BusinessTimeout,
    CleanupTick,
    BlacklistMaintenanceTick,
};

struct TimerEvent {
    TimerEventKind kind = TimerEventKind::IdleTimeout;
    std::uint64_t target_id = 0;
    std::uint32_t target_loop_id = 0;
    int target_fd = -1;
    std::uint64_t target_generation = 0;
    std::uint64_t target_sequence = 0;
};

class ITimerSink {
public:
    virtual ~ITimerSink() = default;

    [[nodiscard]] virtual bool tryPost(TimerEvent event) noexcept = 0;
};

enum class TimerStatus : std::uint8_t {
    Ok,
    InvalidConfig,
    ConfigConflict,
    NotInitialized,
    InvalidState,
    NotRunning,
    Stopped,
    QueueFull,
    TimerLimit,
    SinkExpired,
    InvalidEvent,
    IdExhausted,
    NotFound,
    GenerationMismatch,
    AlreadyCancelled,
    ThreadStartFailed,
    SelfJoin,
    SystemCallFailed,
    WakeFailed,
    InternalError,
};

struct TimerCoreConfig {
    std::size_t command_capacity = 4096;
    std::size_t timer_capacity = 65536;

    [[nodiscard]] constexpr bool operator==(
        const TimerCoreConfig& other
    ) const noexcept {
        return command_capacity == other.command_capacity &&
               timer_capacity == other.timer_capacity;
    }
};

struct TimerScheduleResult {
    TimerStatus status = TimerStatus::NotInitialized;
    TimerId id;
};

class ITimerScheduler {
public:
    virtual ~ITimerScheduler() = default;

    [[nodiscard]] virtual TimerScheduleResult scheduleAt(
        SteadyTime deadline,
        std::weak_ptr<ITimerSink> sink,
        TimerEvent event
    ) noexcept = 0;
    [[nodiscard]] virtual TimerStatus cancel(TimerId id) noexcept = 0;
};

struct TimerCycleResult {
    TimerStatus status = TimerStatus::NotInitialized;
    std::size_t commands_processed = 0;
    std::size_t timers_dispatched = 0;
    std::optional<SteadyTime> next_deadline;
    bool stopped = false;
};

class TimerCore final {
public:
    TimerCore() noexcept = default;
    ~TimerCore() = default;

    TimerCore(const TimerCore&) = delete;
    TimerCore& operator=(const TimerCore&) = delete;
    TimerCore(TimerCore&&) = delete;
    TimerCore& operator=(TimerCore&&) = delete;

    [[nodiscard]] TimerStatus init(const TimerCoreConfig& config) noexcept;
    [[nodiscard]] TimerScheduleResult scheduleAt(
        SteadyTime deadline,
        std::weak_ptr<ITimerSink> sink,
        TimerEvent event
    ) noexcept;
    [[nodiscard]] TimerStatus cancel(TimerId id) noexcept;
    [[nodiscard]] TimerStatus requestStop() noexcept;

    [[nodiscard]] TimerCycleResult processCommands() noexcept;
    [[nodiscard]] TimerCycleResult runReady() noexcept;

private:
    struct Reservation {
        std::uint64_t generation = 0;
        bool cancelled = false;
    };

    struct HeapNode {
        SteadyTime deadline;
        std::uint64_t sequence = 0;
        TimerId id;
        std::weak_ptr<ITimerSink> sink;
        TimerEvent event;
    };

    struct LaterDeadline {
        [[nodiscard]] bool operator()(
            const HeapNode& left,
            const HeapNode& right
        ) const noexcept;
    };

    class TimerHeap final
        : public std::priority_queue<
              HeapNode,
              std::vector<HeapNode>,
              LaterDeadline> {
    public:
        void reserve(const std::size_t capacity) {
            this->c.reserve(capacity);
        }
    };

    enum class CommandKind : std::uint8_t {
        Schedule,
        Cancel,
        Stop,
    };

    struct Command {
        CommandKind kind = CommandKind::Stop;
        HeapNode node;
        TimerId id;
    };

    [[nodiscard]] static bool validConfig(
        const TimerCoreConfig& config
    ) noexcept;
    [[nodiscard]] static bool validEventKind(
        TimerEventKind kind
    ) noexcept;
    void cleanupStaleLocked() noexcept;
    [[nodiscard]] std::optional<SteadyTime>
    nextDeadlineLocked() const noexcept;

    mutable std::mutex mutex_;
    std::optional<TimerCoreConfig> config_;
    std::deque<Command> commands_;
    TimerHeap heap_;
    std::unordered_map<std::uint64_t, Reservation> reservations_;
    std::uint64_t next_id_value_ = 1;
    std::uint64_t next_generation_ = 1;
    std::uint64_t next_sequence_ = 0;
    bool accepting_ = false;
    bool stop_requested_ = false;
    bool stopped_ = false;
};

}  // 命名空间 aegisflow::timer
