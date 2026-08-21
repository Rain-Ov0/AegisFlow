#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace aegisflow::log {

enum class LogLevel : std::uint8_t {
    Debug,
    Info,
    Warn,
    Error,
};

enum class LoggerState : std::uint8_t {
    Constructed,
    Initialized,
    Running,
    Stopping,
    Stopped,
    Failed,
};

enum class LoggerStatus : std::uint8_t {
    Ok,
    InvalidConfig,
    ConfigConflict,
    InvalidState,
    FileOpenFailed,
    ThreadStartFailed,
    SelfJoin,
    JoinFailed,
};

enum class LogSubmitStatus : std::uint8_t {
    Accepted,
    Filtered,
    QueueFull,
    NotRunning,
    AllocationFailed,
};

struct LoggerConfig {
    LogLevel level = LogLevel::Info;
    std::filesystem::path file = "logs/aegisflow.log";
    std::size_t queue_capacity = 4096;
    std::chrono::milliseconds flush_interval{200};

    bool operator==(const LoggerConfig& other) const {
        return level == other.level &&
               file == other.file &&
               queue_capacity == other.queue_capacity &&
               flush_interval == other.flush_interval;
    }
};

struct LogRecord {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level = LogLevel::Info;
    std::thread::id thread_id;
    std::string file;
    std::uint_least32_t line = 0;
    std::string message;
};

struct LoggerStats {
    std::uint64_t accepted = 0;
    std::uint64_t dropped = 0;
    std::uint64_t io_errors = 0;
};

class Logger final {
public:
    static Logger& instance();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    [[nodiscard]] LoggerStatus init(
        const LoggerConfig& config
    ) noexcept;
    [[nodiscard]] LoggerStatus start() noexcept;
    [[nodiscard]] bool shouldLog(LogLevel level) const noexcept;
    [[nodiscard]] LogSubmitStatus log(
        LogLevel level,
        std::string_view message,
        std::string_view file,
        std::uint_least32_t line
    ) noexcept;
    void stop() noexcept;
    [[nodiscard]] LoggerStatus join() noexcept;

    [[nodiscard]] LoggerState state() const noexcept;
    [[nodiscard]] LoggerStats stats() const noexcept;

private:
    Logger() noexcept = default;
    ~Logger();

    [[nodiscard]] static bool validConfig(
        const LoggerConfig& config
    ) noexcept;
    void workerLoop() noexcept;
    [[nodiscard]] bool writeRecord(const LogRecord& record) noexcept;
    [[nodiscard]] bool writeDroppedSummary(
        std::uint64_t count
    ) noexcept;
    void writePendingDroppedSummary() noexcept;
    [[nodiscard]] bool flushOutput() noexcept;
    void recordIoError() noexcept;
    void setStateLocked(LoggerState state) noexcept;

    mutable std::mutex mutex_;
    std::condition_variable queue_ready_;
    std::condition_variable state_changed_;
    std::optional<LoggerConfig> config_;
    std::deque<LogRecord> queue_;
    std::thread worker_;
    std::ofstream output_;
    std::atomic<LogLevel> minimum_level_{LogLevel::Info};
    std::atomic<LoggerState> state_{LoggerState::Constructed};
    std::atomic<std::uint64_t> accepted_count_{0};
    std::atomic<std::uint64_t> dropped_count_{0};
    std::atomic<std::uint64_t> io_error_count_{0};
    std::uint64_t pending_dropped_ = 0;
    LoggerStatus join_status_ = LoggerStatus::Ok;
    bool join_in_progress_ = false;
    bool output_dirty_ = false;
    bool io_error_reported_ = false;
};

}  // 命名空间 aegisflow::log

#define AEGISFLOW_LOG_AT(level_value, message_value)                       \
    do {                                                                  \
        auto& aegisflow_logger = ::aegisflow::log::Logger::instance();    \
        if (aegisflow_logger.shouldLog(level_value)) {                    \
            (void)aegisflow_logger.log(                                   \
                level_value, message_value, __FILE__, __LINE__);          \
        }                                                                 \
    } while (false)

#define AEGISFLOW_LOG_DEBUG(message_value)                                \
    AEGISFLOW_LOG_AT(::aegisflow::log::LogLevel::Debug, message_value)
#define AEGISFLOW_LOG_INFO(message_value)                                 \
    AEGISFLOW_LOG_AT(::aegisflow::log::LogLevel::Info, message_value)
#define AEGISFLOW_LOG_WARN(message_value)                                 \
    AEGISFLOW_LOG_AT(::aegisflow::log::LogLevel::Warn, message_value)
#define AEGISFLOW_LOG_ERROR(message_value)                                \
    AEGISFLOW_LOG_AT(::aegisflow::log::LogLevel::Error, message_value)
