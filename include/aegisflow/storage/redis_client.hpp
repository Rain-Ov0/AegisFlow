#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <mutex>

struct redisContext;

namespace aegisflow::storage {

struct RedisConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 6379;
    std::string password = "123456";
    unsigned int timeout_ms = 50;
};

class RedisClient {
public:
    RedisClient();
    explicit RedisClient(RedisConfig config);
    ~RedisClient();

    RedisClient(const RedisClient&) = delete;
    RedisClient& operator=(const RedisClient&) = delete;

    bool connect();
    bool connect(const RedisConfig& config);
    void close();

    std::optional<std::string> get(const std::string& key);
    bool setex(const std::string& key, int ttl_sec, const std::string& value);
    bool ping();

    [[nodiscard]] bool available() const;
    [[nodiscard]] const std::string& lastError() const;

private:
    void markUnavailable(const std::string& error);

private:
    RedisConfig config_;
    redisContext* conn_ = nullptr;
    std::atomic<bool> available_{false};
    std::string last_error_;
    std::mutex mutex_;
};

} // namespace aegisflow::storage