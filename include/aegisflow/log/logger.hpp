#pragma once

#include <string>
#include <mutex>
#include <fstream>

namespace aegisflow::log {

enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR
};

class Logger {
public:
    static Logger& instance();

    void init(LogLevel level, const std::string& file);
    void log(LogLevel level, const std::string& msg);
    LogLevel stringToLogLevel(const std::string& level) const;

private:
    Logger() = default;
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

private:
    std::mutex mutex_;
    std::ofstream file_;
    LogLevel level_ = LogLevel::INFO;
};

#define LOG_INFO(msg) aegisflow::log::Logger::instance().log(aegisflow::log::LogLevel::INFO, msg)
#define LOG_WARN(msg) aegisflow::log::Logger::instance().log(aegisflow::log::LogLevel::WARN, msg)
#define LOG_ERROR(msg) aegisflow::log::Logger::instance().log(aegisflow::log::LogLevel::ERROR, msg)

} //namespace aegisflow::log