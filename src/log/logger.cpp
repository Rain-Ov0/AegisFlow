#include "aegisflow/log/logger.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <limits>
#include <system_error>
#include <utility>

namespace aegisflow::log {
namespace {

thread_local const Logger* active_logger_service = nullptr;

[[nodiscard]] constexpr std::string_view levelName(
    const LogLevel level
) noexcept {
    switch (level) {
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
    }
    return "UNKNOWN";
}

void writePrefix(
    std::ofstream& output,
    const std::chrono::system_clock::time_point timestamp,
    const LogLevel level,
    const std::thread::id thread_id,
    const std::string_view file,
    const std::uint_least32_t line
) noexcept {
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(
        timestamp
    );
    const auto milliseconds = std::chrono::duration_cast<
        std::chrono::milliseconds>(timestamp - seconds).count();
    const std::time_t calendar_time =
        std::chrono::system_clock::to_time_t(timestamp);
    std::tm utc{};
    if (::gmtime_r(&calendar_time, &utc) == nullptr) {
        output << "[time-unavailable]";
    } else {
        output << '[' << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
               << std::setfill('0') << std::setw(3) << milliseconds
               << "Z]";
    }
    output << '[' << levelName(level) << "]"
           << "[thread=" << thread_id << "] " << file << ':' << line
           << ' ';
}

}  // 命名空间

Logger::~Logger() {
    stop();
    (void)join();
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

bool Logger::validConfig(const LoggerConfig& config) noexcept {
    return !config.file.empty() && config.queue_capacity > 0 &&
           config.flush_interval.count() > 0 &&
           config.flush_interval <= std::chrono::hours(24);
}

LoggerStatus Logger::init(const LoggerConfig& config) noexcept {
    if (!validConfig(config)) {
        return LoggerStatus::InvalidConfig;
    }

    std::lock_guard lock(mutex_);
    if (config_.has_value()) {
        return *config_ == config
                   ? LoggerStatus::Ok
                   : LoggerStatus::ConfigConflict;
    }
    if (state_.load(std::memory_order_relaxed) !=
        LoggerState::Constructed) {
        return LoggerStatus::InvalidState;
    }

    try {
        config_ = config;
    } catch (...) {
        return LoggerStatus::InvalidConfig;
    }
    minimum_level_.store(config.level, std::memory_order_release);
    setStateLocked(LoggerState::Initialized);
    return LoggerStatus::Ok;
}

LoggerStatus Logger::start() noexcept {
    std::lock_guard lock(mutex_);
    const auto current = state_.load(std::memory_order_relaxed);
    if (current == LoggerState::Running) {
        return LoggerStatus::Ok;
    }
    if (current != LoggerState::Initialized || !config_.has_value()) {
        return LoggerStatus::InvalidState;
    }

    const auto parent = config_->file.parent_path();
    if (!parent.empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(parent, directory_error);
        if (directory_error) {
            setStateLocked(LoggerState::Failed);
            return LoggerStatus::FileOpenFailed;
        }
    }

    output_.open(config_->file, std::ios::out | std::ios::app);
    if (!output_.is_open()) {
        setStateLocked(LoggerState::Failed);
        return LoggerStatus::FileOpenFailed;
    }

    try {
        worker_ = std::thread(&Logger::workerLoop, this);
    } catch (...) {
        output_.close();
        setStateLocked(LoggerState::Failed);
        return LoggerStatus::ThreadStartFailed;
    }
    setStateLocked(LoggerState::Running);
    return LoggerStatus::Ok;
}

bool Logger::shouldLog(const LogLevel level) const noexcept {
    return state_.load(std::memory_order_acquire) == LoggerState::Running &&
           level >= minimum_level_.load(std::memory_order_relaxed);
}

LogSubmitStatus Logger::log(
    const LogLevel level,
    const std::string_view message,
    const std::string_view file,
    const std::uint_least32_t line
) noexcept {
    if (state_.load(std::memory_order_acquire) != LoggerState::Running) {
        return LogSubmitStatus::NotRunning;
    }
    if (level < minimum_level_.load(std::memory_order_relaxed)) {
        return LogSubmitStatus::Filtered;
    }

    try {
        LogRecord record;
        record.timestamp = std::chrono::system_clock::now();
        record.level = level;
        record.thread_id = std::this_thread::get_id();
        record.file = std::string(file);
        record.line = line;
        record.message = std::string(message);

        std::unique_lock lock(mutex_);
        if (state_.load(std::memory_order_relaxed) !=
            LoggerState::Running) {
            return LogSubmitStatus::NotRunning;
        }
        if (queue_.size() >= config_->queue_capacity) {
            ++pending_dropped_;
            dropped_count_.fetch_add(1, std::memory_order_relaxed);
            return LogSubmitStatus::QueueFull;
        }
        queue_.push_back(std::move(record));
        accepted_count_.fetch_add(1, std::memory_order_relaxed);
        lock.unlock();
        queue_ready_.notify_one();
        return LogSubmitStatus::Accepted;
    } catch (...) {
        return LogSubmitStatus::AllocationFailed;
    }
}

void Logger::stop() noexcept {
    bool notify_worker = false;
    {
        std::lock_guard lock(mutex_);
        const auto current = state_.load(std::memory_order_relaxed);
        if (current == LoggerState::Initialized) {
            setStateLocked(LoggerState::Stopped);
            state_changed_.notify_all();
            return;
        }
        if (current != LoggerState::Running) {
            return;
        }
        setStateLocked(LoggerState::Stopping);
        notify_worker = true;
    }
    if (notify_worker) {
        queue_ready_.notify_all();
    }
}

LoggerStatus Logger::join() noexcept {
    if (active_logger_service == this) {
        return LoggerStatus::SelfJoin;
    }

    std::unique_lock lock(mutex_);
    const auto current = state_.load(std::memory_order_relaxed);
    if (current == LoggerState::Constructed ||
        current == LoggerState::Initialized ||
        current == LoggerState::Running) {
        return LoggerStatus::InvalidState;
    }
    if (join_in_progress_) {
        state_changed_.wait(lock, [this] { return !join_in_progress_; });
        return join_status_;
    }
    if (!worker_.joinable()) {
        return join_status_;
    }

    join_in_progress_ = true;
    std::thread worker = std::move(worker_);
    lock.unlock();

    LoggerStatus result = LoggerStatus::Ok;
    try {
        worker.join();
    } catch (...) {
        result = LoggerStatus::JoinFailed;
    }

    lock.lock();
    join_status_ = result;
    join_in_progress_ = false;
    state_changed_.notify_all();
    return result;
}

LoggerState Logger::state() const noexcept {
    return state_.load(std::memory_order_acquire);
}

LoggerStats Logger::stats() const noexcept {
    LoggerStats result;
    result.accepted = accepted_count_.load(std::memory_order_relaxed);
    result.dropped = dropped_count_.load(std::memory_order_relaxed);
    result.io_errors = io_error_count_.load(std::memory_order_relaxed);
    return result;
}

void Logger::workerLoop() noexcept {
    active_logger_service = this;
    auto next_flush = std::chrono::steady_clock::now() +
                      config_->flush_interval;

    while (true) {
        std::deque<LogRecord> batch;
        bool stopping = false;
        {
            std::unique_lock lock(mutex_);
            (void)queue_ready_.wait_until(lock, next_flush, [this] {
                return !queue_.empty() ||
                       state_.load(std::memory_order_relaxed) ==
                           LoggerState::Stopping;
            });
            batch.swap(queue_);
            stopping = state_.load(std::memory_order_relaxed) ==
                       LoggerState::Stopping;
        }

        for (const auto& record : batch) {
            if (writeRecord(record)) {
                writePendingDroppedSummary();
            }
            if (record.level == LogLevel::Error) {
                (void)flushOutput();
                next_flush = std::chrono::steady_clock::now() +
                             config_->flush_interval;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_flush) {
            (void)flushOutput();
            next_flush = now + config_->flush_interval;
        }

        if (stopping) {
            std::lock_guard lock(mutex_);
            if (queue_.empty()) {
                break;
            }
        }
    }

    (void)flushOutput();
    const bool close_was_expected_to_succeed = output_.good();
    output_.close();
    if (close_was_expected_to_succeed && output_.fail()) {
        recordIoError();
    }
    {
        std::lock_guard lock(mutex_);
        setStateLocked(LoggerState::Stopped);
        state_changed_.notify_all();
    }
    active_logger_service = nullptr;
}

bool Logger::writeRecord(const LogRecord& record) noexcept {
    writePrefix(
        output_, record.timestamp, record.level, record.thread_id,
        record.file, record.line
    );
    output_ << record.message << '\n';
    if (!output_) {
        recordIoError();
        return false;
    }
    output_dirty_ = true;
    return true;
}

bool Logger::writeDroppedSummary(const std::uint64_t count) noexcept {
    writePrefix(
        output_, std::chrono::system_clock::now(), LogLevel::Warn,
        std::this_thread::get_id(), "logger", 0
    );
    output_ << "dropped_count=" << count << '\n';
    if (!output_) {
        recordIoError();
        return false;
    }
    output_dirty_ = true;
    return true;
}

void Logger::writePendingDroppedSummary() noexcept {
    std::uint64_t pending = 0;
    {
        std::lock_guard lock(mutex_);
        pending = std::exchange(pending_dropped_, 0);
    }
    if (pending == 0 || writeDroppedSummary(pending)) {
        return;
    }
    std::lock_guard lock(mutex_);
    if (pending > std::numeric_limits<std::uint64_t>::max() -
                      pending_dropped_) {
        pending_dropped_ = std::numeric_limits<std::uint64_t>::max();
    } else {
        pending_dropped_ += pending;
    }
}

bool Logger::flushOutput() noexcept {
    if (!output_dirty_) {
        return true;
    }
    output_.flush();
    output_dirty_ = false;
    if (!output_) {
        recordIoError();
        return false;
    }
    return true;
}

void Logger::recordIoError() noexcept {
    io_error_count_.fetch_add(1, std::memory_order_relaxed);
    if (io_error_reported_) {
        return;
    }
    io_error_reported_ = true;
    std::fputs("AegisFlow Logger: log file write failed\n", stderr);
    std::fflush(stderr);
}

void Logger::setStateLocked(const LoggerState state) noexcept {
    state_.store(state, std::memory_order_release);
}

}  // 命名空间 aegisflow::log
