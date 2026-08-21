#include "aegisflow/timer/timer.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <limits>
#include <utility>

namespace aegisflow::timer {
namespace {

constexpr std::uint64_t kCommandToken = 1;
constexpr std::uint64_t kTimerToken = 2;
thread_local const Timer* active_timer_service = nullptr;

int createEpoll() noexcept {
    int fd = -1;
    do {
        fd = ::epoll_create1(EPOLL_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

int createEventFd() noexcept {
    int fd = -1;
    do {
        fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

int createTimerFd() noexcept {
    int fd = -1;
    do {
        fd = ::timerfd_create(
            CLOCK_MONOTONIC,
            TFD_NONBLOCK | TFD_CLOEXEC
        );
    } while (fd < 0 && errno == EINTR);
    return fd;
}

bool registerFd(const int epoll_fd, const int fd,
                const std::uint64_t token) noexcept {
    epoll_event event{};
    event.events = EPOLLIN | EPOLLET;
    event.data.u64 = token;
    int result = -1;
    do {
        result = ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event);
    } while (result < 0 && errno == EINTR);
    return result == 0;
}

void closeFd(const int fd) noexcept {
    if (fd >= 0) {
        (void)::close(fd);
    }
}

}  // 命名空间

Timer::~Timer() {
    stop();
    (void)join();
    closeFds();
}

Timer& Timer::instance() {
    static Timer timer;
    return timer;
}

bool Timer::validConfig(const TimerConfig& config) noexcept {
    return config.command_capacity > 0 && config.timer_capacity > 0;
}

TimerStatus Timer::openFds() noexcept {
    const int epoll_fd = createEpoll();
    if (epoll_fd < 0) {
        return TimerStatus::SystemCallFailed;
    }
    const int event_fd = createEventFd();
    if (event_fd < 0) {
        closeFd(epoll_fd);
        return TimerStatus::SystemCallFailed;
    }
    const int timer_fd = createTimerFd();
    if (timer_fd < 0 ||
        !registerFd(epoll_fd, event_fd, kCommandToken) ||
        !registerFd(epoll_fd, timer_fd, kTimerToken)) {
        closeFd(timer_fd);
        closeFd(event_fd);
        closeFd(epoll_fd);
        return TimerStatus::SystemCallFailed;
    }

    std::lock_guard lock(fd_mutex_);
    epoll_fd_ = epoll_fd;
    event_fd_ = event_fd;
    timer_fd_ = timer_fd;
    return TimerStatus::Ok;
}

void Timer::closeFds() noexcept {
    int epoll_fd = -1;
    int event_fd = -1;
    int timer_fd = -1;
    {
        std::lock_guard lock(fd_mutex_);
        epoll_fd = std::exchange(epoll_fd_, -1);
        event_fd = std::exchange(event_fd_, -1);
        timer_fd = std::exchange(timer_fd_, -1);
    }
    closeFd(timer_fd);
    closeFd(event_fd);
    closeFd(epoll_fd);
}

TimerStatus Timer::notifyWorker() noexcept {
    std::lock_guard lock(fd_mutex_);
    if (event_fd_ < 0) {
        return TimerStatus::WakeFailed;
    }
    constexpr std::uint64_t value = 1;
    while (true) {
        const auto written = ::write(event_fd_, &value, sizeof(value));
        if (written == static_cast<ssize_t>(sizeof(value)) ||
            (written < 0 && errno == EAGAIN)) {
            return TimerStatus::Ok;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return TimerStatus::WakeFailed;
    }
}

Timer::WaitResult Timer::waitForEvent() noexcept {
    int epoll_fd = -1;
    {
        std::lock_guard lock(fd_mutex_);
        epoll_fd = epoll_fd_;
    }
    if (epoll_fd < 0) {
        WaitResult result;
        result.status = TimerStatus::SystemCallFailed;
        return result;
    }

    std::array<epoll_event, 2> events{};
    const int ready = ::epoll_wait(
        epoll_fd,
        events.data(),
        static_cast<int>(events.size()),
        -1
    );
    if (ready < 0) {
        WaitResult result;
        if (errno == EINTR) {
            result.interrupted = true;
        } else {
            result.status = TimerStatus::SystemCallFailed;
        }
        return result;
    }

    WaitResult result;
    for (int index = 0; index < ready; ++index) {
        const auto& event = events[static_cast<std::size_t>(index)];
        if ((event.events & (EPOLLERR | EPOLLHUP)) != 0U) {
            WaitResult failure;
            failure.status = TimerStatus::SystemCallFailed;
            return failure;
        }
        if (event.data.u64 == kCommandToken) {
            result.command_ready = true;
        } else if (event.data.u64 == kTimerToken) {
            result.timer_ready = true;
        } else {
            WaitResult failure;
            failure.status = TimerStatus::SystemCallFailed;
            return failure;
        }
    }
    return result;
}

TimerStatus Timer::drainFd(const int fd) noexcept {
    if (fd < 0) {
        return TimerStatus::SystemCallFailed;
    }
    while (true) {
        std::uint64_t value = 0;
        const auto count = ::read(fd, &value, sizeof(value));
        if (count == static_cast<ssize_t>(sizeof(value))) {
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return count < 0 && errno == EAGAIN
                   ? TimerStatus::Ok
                   : TimerStatus::SystemCallFailed;
    }
}

TimerStatus Timer::arm(const std::optional<SteadyTime> deadline) noexcept {
    std::lock_guard lock(fd_mutex_);
    if (timer_fd_ < 0) {
        return TimerStatus::SystemCallFailed;
    }

    itimerspec specification{};
    if (deadline.has_value()) {
        auto delay = std::chrono::duration_cast<std::chrono::nanoseconds>(
            *deadline - SteadyClock::now()
        );
        if (delay <= std::chrono::nanoseconds::zero()) {
            delay = std::chrono::nanoseconds(1);
        }
        const auto seconds = std::chrono::duration_cast<
            std::chrono::seconds>(delay);
        const auto remainder = delay - seconds;
        if (seconds.count() > static_cast<std::int64_t>(
                                  std::numeric_limits<time_t>::max())) {
            specification.it_value.tv_sec =
                std::numeric_limits<time_t>::max();
            specification.it_value.tv_nsec = 999999999;
        } else {
            specification.it_value.tv_sec =
                static_cast<time_t>(seconds.count());
            specification.it_value.tv_nsec =
                static_cast<long>(remainder.count());
        }
    }

    int result = -1;
    do {
        result = ::timerfd_settime(timer_fd_, 0, &specification, nullptr);
    } while (result < 0 && errno == EINTR);
    return result == 0 ? TimerStatus::Ok : TimerStatus::SystemCallFailed;
}

TimerStatus Timer::init(const TimerConfig& config) noexcept {
    if (!validConfig(config)) {
        return TimerStatus::InvalidConfig;
    }

    std::lock_guard lock(mutex_);
    if (config_.has_value()) {
        return *config_ == config
                   ? TimerStatus::Ok
                   : TimerStatus::ConfigConflict;
    }
    if (state_.load(std::memory_order_relaxed) != TimerState::Constructed) {
        return TimerStatus::InvalidState;
    }
    if (openFds() != TimerStatus::Ok) {
        setStateLocked(TimerState::Failed);
        return TimerStatus::SystemCallFailed;
    }
    TimerCoreConfig scheduler_config;
    scheduler_config.command_capacity = config.command_capacity;
    scheduler_config.timer_capacity = config.timer_capacity;
    const auto scheduler_status = scheduler_.init(scheduler_config);
    if (scheduler_status != TimerStatus::Ok) {
        closeFds();
        setStateLocked(TimerState::Failed);
        return scheduler_status;
    }
    config_ = config;
    setStateLocked(TimerState::Initialized);
    return TimerStatus::Ok;
}

TimerStatus Timer::start() noexcept {
    std::unique_lock lock(mutex_);
    const auto current = state_.load(std::memory_order_relaxed);
    if (current == TimerState::Running) {
        return TimerStatus::Ok;
    }
    if (current == TimerState::Starting) {
        state_changed_.wait(lock, [this] {
            return state_.load(std::memory_order_relaxed) !=
                   TimerState::Starting;
        });
        return state_.load(std::memory_order_relaxed) == TimerState::Running
                   ? TimerStatus::Ok
                   : start_status_;
    }
    if (current != TimerState::Initialized) {
        return TimerStatus::InvalidState;
    }

    start_status_ = TimerStatus::InternalError;
    setStateLocked(TimerState::Starting);
    try {
        worker_ = std::thread(&Timer::workerLoop, this);
    } catch (...) {
        closeFds();
        start_status_ = TimerStatus::ThreadStartFailed;
        setStateLocked(TimerState::Failed);
        return start_status_;
    }
    state_changed_.wait(lock, [this] {
        return state_.load(std::memory_order_relaxed) != TimerState::Starting;
    });
    return state_.load(std::memory_order_relaxed) == TimerState::Running
               ? TimerStatus::Ok
               : start_status_;
}

void Timer::stop() noexcept {
    bool without_worker = false;
    {
        std::lock_guard lock(mutex_);
        const auto current = state_.load(std::memory_order_relaxed);
        if (current == TimerState::Constructed ||
            current == TimerState::Stopping ||
            current == TimerState::Stopped ||
            current == TimerState::Failed) {
            return;
        }
        without_worker = current == TimerState::Initialized;
        setStateLocked(TimerState::Stopping);
        (void)scheduler_.requestStop();
    }
    if (without_worker) {
        (void)scheduler_.processCommands();
        (void)arm(std::nullopt);
        closeFds();
        std::lock_guard lock(mutex_);
        setStateLocked(TimerState::Stopped);
        state_changed_.notify_all();
        return;
    }
    (void)notifyWorker();
}

TimerStatus Timer::join() noexcept {
    std::unique_lock lock(mutex_);
    if (active_timer_service == this) {
        return TimerStatus::SelfJoin;
    }
    const auto current = state_.load(std::memory_order_relaxed);
    if (current == TimerState::Constructed ||
        current == TimerState::Initialized ||
        current == TimerState::Starting ||
        current == TimerState::Running) {
        return TimerStatus::InvalidState;
    }
    if (join_in_progress_) {
        state_changed_.wait(lock, [this] { return !join_in_progress_; });
        return join_status_;
    }
    if (!worker_.joinable()) {
        return TimerStatus::Ok;
    }

    join_in_progress_ = true;
    std::thread worker = std::move(worker_);
    lock.unlock();
    worker.join();
    lock.lock();
    join_status_ = TimerStatus::Ok;
    join_in_progress_ = false;
    state_changed_.notify_all();
    return join_status_;
}

TimerScheduleResult Timer::scheduleAt(
    const SteadyTime deadline,
    std::weak_ptr<ITimerSink> sink,
    const TimerEvent event
) noexcept {
    if (state_.load(std::memory_order_acquire) != TimerState::Running) {
        const auto current = state_.load(std::memory_order_relaxed);
        return {
            current == TimerState::Stopping || current == TimerState::Stopped
                ? TimerStatus::Stopped
                : TimerStatus::NotRunning,
            {},
        };
    }
    auto result = scheduler_.scheduleAt(deadline, std::move(sink), event);
    if (result.status == TimerStatus::Ok &&
        notifyWorker() != TimerStatus::Ok) {
        result.status = TimerStatus::WakeFailed;
    }
    return result;
}

TimerStatus Timer::cancel(const TimerId id) noexcept {
    if (state_.load(std::memory_order_acquire) != TimerState::Running) {
        const auto current = state_.load(std::memory_order_relaxed);
        return current == TimerState::Stopping || current == TimerState::Stopped
                   ? TimerStatus::Stopped
                   : TimerStatus::NotRunning;
    }
    const auto result = scheduler_.cancel(id);
    if (result != TimerStatus::Ok) {
        return result;
    }
    return notifyWorker();
}

void Timer::workerLoop() noexcept {
    active_timer_service = this;
    {
        std::lock_guard lock(mutex_);
        start_status_ = TimerStatus::Ok;
        setStateLocked(TimerState::Running);
        state_changed_.notify_all();
    }

    bool failed = false;
    while (true) {
        const auto cycle = scheduler_.runReady();
        if (cycle.status != TimerStatus::Ok) {
            failed = true;
            break;
        }
        if (cycle.stopped) {
            break;
        }
        if (arm(cycle.next_deadline) != TimerStatus::Ok) {
            failed = true;
            break;
        }
        const auto ready = waitForEvent();
        if (ready.status != TimerStatus::Ok) {
            failed = true;
            break;
        }
        if (ready.interrupted) {
            continue;
        }

        int event_fd = -1;
        int timer_fd = -1;
        {
            std::lock_guard lock(fd_mutex_);
            event_fd = event_fd_;
            timer_fd = timer_fd_;
        }
        if ((ready.command_ready && drainFd(event_fd) != TimerStatus::Ok) ||
            (ready.timer_ready && drainFd(timer_fd) != TimerStatus::Ok)) {
            failed = true;
            break;
        }
    }

    if (failed) {
        (void)scheduler_.requestStop();
        (void)scheduler_.runReady();
    }
    if (arm(std::nullopt) != TimerStatus::Ok) {
        failed = true;
    }
    closeFds();
    {
        std::lock_guard lock(mutex_);
        setStateLocked(failed ? TimerState::Failed : TimerState::Stopped);
        state_changed_.notify_all();
    }
    active_timer_service = nullptr;
}

void Timer::setStateLocked(const TimerState state) noexcept {
    state_.store(state, std::memory_order_release);
}

}  // 命名空间 aegisflow::timer
