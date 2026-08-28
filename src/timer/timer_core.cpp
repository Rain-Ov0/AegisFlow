#include "aegisflow/timer/timer_core.hpp"

#include <limits>
#include <utility>

namespace aegisflow::timer {

bool TimerCore::LaterDeadline::operator()(
    const HeapNode& left,
    const HeapNode& right
) const noexcept {
    if (left.deadline != right.deadline) {
        return left.deadline > right.deadline;
    }
    return left.sequence > right.sequence;
}

bool TimerCore::validConfig(const TimerCoreConfig& config) noexcept {
    return config.command_capacity > 0 && config.timer_capacity > 0;
}

bool TimerCore::validEventKind(const TimerEventKind kind) noexcept {
    switch (kind) {
        case TimerEventKind::IdleTimeout:
        case TimerEventKind::ReadTimeout:
        case TimerEventKind::WriteTimeout:
        case TimerEventKind::BusinessTimeout:
        case TimerEventKind::CleanupTick:
        case TimerEventKind::BlacklistMaintenanceTick:
            return true;
    }
    return false;
}

TimerStatus TimerCore::init(const TimerCoreConfig& config) noexcept {
    if (!validConfig(config)) {
        return TimerStatus::InvalidConfig;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (config_.has_value()) {
        return *config_ == config
            ? TimerStatus::Ok
            : TimerStatus::ConfigConflict;
    }

    try {
        heap_.reserve(config.timer_capacity);
        std::unordered_map<std::uint64_t, Reservation> new_reservations;
        new_reservations.reserve(config.timer_capacity);
        reservations_ = std::move(new_reservations);
    } catch (...) {
        return TimerStatus::InternalError;
    }

    config_ = config;
    accepting_ = true;
    stop_requested_ = false;
    stopped_ = false;
    return TimerStatus::Ok;
}

TimerScheduleResult TimerCore::scheduleAt(
    const SteadyTime deadline,
    std::weak_ptr<ITimerSink> sink,
    const TimerEvent event
) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.has_value()) {
        return {TimerStatus::NotInitialized, {}};
    }
    if (!accepting_) {
        return {TimerStatus::Stopped, {}};
    }
    if (!validEventKind(event.kind)) {
        return {TimerStatus::InvalidEvent, {}};
    }
    if (sink.expired()) {
        return {TimerStatus::SinkExpired, {}};
    }
    if (commands_.size() >= config_->command_capacity) {
        return {TimerStatus::QueueFull, {}};
    }
    if (reservations_.size() >= config_->timer_capacity) {
        return {TimerStatus::TimerLimit, {}};
    }
    if (next_id_value_ == std::numeric_limits<std::uint64_t>::max() ||
        next_generation_ == std::numeric_limits<std::uint64_t>::max() ||
        next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        return {TimerStatus::IdExhausted, {}};
    }

    TimerId id;
    id.value = next_id_value_;
    id.generation = next_generation_;
    HeapNode node;
    node.deadline = deadline;
    node.sequence = next_sequence_;
    node.id = id;
    node.sink = std::move(sink);
    node.event = event;

    try {
        // 先保留 id 再入命令队列，cancel 因此能与尚未入堆的 schedule 正确竞争。
        Reservation new_reservation;
        new_reservation.generation = id.generation;
        new_reservation.cancelled = false;
        const auto [reservation, inserted] = reservations_.emplace(
            id.value,
            new_reservation
        );
        if (!inserted) {
            return {TimerStatus::InternalError, {}};
        }
        try {
            Command command;
            command.kind = CommandKind::Schedule;
            command.node = std::move(node);
            command.id = id;
            commands_.push_back(std::move(command));
        } catch (...) {
            reservations_.erase(reservation);
            throw;
        }
    } catch (...) {
        return {TimerStatus::InternalError, {}};
    }

    ++next_id_value_;
    ++next_generation_;
    ++next_sequence_;
    return {TimerStatus::Ok, id};
}

TimerStatus TimerCore::cancel(const TimerId id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.has_value()) {
        return TimerStatus::NotInitialized;
    }
    if (!accepting_) {
        return TimerStatus::Stopped;
    }

    const auto reservation = reservations_.find(id.value);
    if (reservation == reservations_.end()) {
        return TimerStatus::NotFound;
    }
    if (reservation->second.generation != id.generation) {
        return TimerStatus::GenerationMismatch;
    }
    // cancel 本身已持有 core mutex，直接释放 reservation 即可。
    // 堆中可能已存在的节点会因 reservation 缺失而惰性跳过；
    // 尚未入堆的 schedule command 也会在消费时被跳过。
    // 这使取消不再占用 command queue，也不会让已取消
    // Timer 长时间占用 timer_capacity。
    reservations_.erase(reservation);
    return TimerStatus::Ok;
}

TimerStatus TimerCore::requestStop() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.has_value()) {
        return TimerStatus::NotInitialized;
    }
    if (stop_requested_) {
        return TimerStatus::Ok;
    }

    try {
        Command command;
        command.kind = CommandKind::Stop;
        commands_.push_back(std::move(command));
    } catch (...) {
        return TimerStatus::InternalError;
    }

    accepting_ = false;
    stop_requested_ = true;
    return TimerStatus::Ok;
}

TimerCycleResult TimerCore::processCommands() noexcept {
    TimerCycleResult result;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.has_value()) {
        result.status = TimerStatus::NotInitialized;
        return result;
    }

    result.status = TimerStatus::Ok;
    while (!commands_.empty()) {
        Command& command = commands_.front();
        if (command.kind == CommandKind::Schedule) {
            const auto reservation = reservations_.find(command.id.value);
            const bool still_active =
                reservation != reservations_.end() &&
                reservation->second.generation == command.id.generation &&
                !reservation->second.cancelled;
            if (still_active) {
                try {
                    heap_.push(command.node);
                } catch (...) {
                    result.status = TimerStatus::InternalError;
                    break;
                }
            }
        } else if (command.kind == CommandKind::Stop) {
            for (auto& [unused_id, reservation] : reservations_) {
                static_cast<void>(unused_id);
                reservation.cancelled = true;
            }
            stopped_ = true;
        }

        commands_.pop_front();
        ++result.commands_processed;
    }

    cleanupStaleLocked();
    result.next_deadline = nextDeadlineLocked();
    result.stopped = stopped_;
    return result;
}

TimerCycleResult TimerCore::runReady() noexcept {
    TimerCycleResult result = processCommands();
    if (result.status != TimerStatus::Ok) {
        return result;
    }

    const SteadyTime now = SteadyClock::now();
    while (true) {
        HeapNode node;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_requested_) {
                break;
            }
            cleanupStaleLocked();
            if (heap_.empty() || heap_.top().deadline > now) {
                break;
            }

            node = heap_.top();
            heap_.pop();
            const auto reservation = reservations_.find(node.id.value);
            if (reservation == reservations_.end() ||
                reservation->second.generation != node.id.generation ||
                reservation->second.cancelled) {
                continue;
            }
            reservations_.erase(reservation);
        }

        const auto sink = node.sink.lock();
        if (sink) {
            static_cast<void>(sink->tryPost(node.event));
        }
        ++result.timers_dispatched;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        cleanupStaleLocked();
        result.next_deadline = nextDeadlineLocked();
        result.stopped = stopped_;
    }
    return result;
}

void TimerCore::cleanupStaleLocked() noexcept {
    // 取消采用惰性删除；generation 防止 id 复用时的旧堆节点误命中新 Timer。
    while (!heap_.empty()) {
        const HeapNode& node = heap_.top();
        const auto reservation = reservations_.find(node.id.value);
        const bool active =
            reservation != reservations_.end() &&
            reservation->second.generation == node.id.generation &&
            !reservation->second.cancelled;
        if (active) {
            return;
        }

        if (reservation != reservations_.end() &&
            reservation->second.generation == node.id.generation &&
            reservation->second.cancelled) {
            reservations_.erase(reservation);
        }
        heap_.pop();
    }
}

std::optional<SteadyTime> TimerCore::nextDeadlineLocked() const noexcept {
    if (heap_.empty()) {
        return std::nullopt;
    }
    return heap_.top().deadline;
}

}  // 命名空间 aegisflow::timer
