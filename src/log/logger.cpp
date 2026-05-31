#include "logger.hpp"

#include <string>
#include <chrono>
#include <sstream>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <mutex>

namespace {
    // 日志级别转换为字符串
    std::string logLevelToString(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG: 
                return "DEBUG";
            case LogLevel::INFO:
                return "INFO";
            case LogLevel::WARN:
                return "WARN";
            case LogLevel::ERROR:
                return "ERROR";
            default:
                return "UNKNOWN";
        }
    }

    // 获取当前时间字符串
    std::string currentTimeString() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);

        std::tm tm{};
        
        #if defined(_WIN32) 
        localtime_s(&tm, &time);
        #else
        localtime_r(&time, &tm);
        #endif 

        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");

        return oss.str();
    }
} //namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::init(LogLevel level, const std::string& file) {
    std::lock_guard<std::mutex> lock(mutex_);

    level_ = level;

    if (file_.is_open()) {
        file_.close();
    }

    file_.open(file, std::ios::app);

    if (!file_.is_open()) {
        std::cerr << "Failed to open log file: " << file << std::endl;
    }
}

void Logger::log(LogLevel level, const std::string& msg) {
    if (level < level_) {
        return ;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    std::ostringstream oss;
    oss << currentTimeString()
        << "[" << logLevelToString(level) << "] " 
        << msg << '\n';

    if (file_.is_open()) {
        file_ << oss.str();
        file_.flush();
    } else {
        std::cerr << oss.str() << std::endl;
    }
}