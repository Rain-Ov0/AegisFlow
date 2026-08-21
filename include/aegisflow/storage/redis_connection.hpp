#pragma once

#include "aegisflow/base/array_view.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aegisflow::storage {

using RedisDeadline = std::chrono::steady_clock::time_point;

struct RedisConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6379;
    std::string username;
    std::string password;
    std::uint32_t database = 0;
    std::chrono::milliseconds connect_timeout{1000};
    std::chrono::milliseconds command_timeout{1000};
    std::string key_prefix = "aegisflow:blacklist";

    bool operator==(const RedisConfig& other) const {
        return host == other.host &&
               port == other.port &&
               username == other.username &&
               password == other.password &&
               database == other.database &&
               connect_timeout == other.connect_timeout &&
               command_timeout == other.command_timeout &&
               key_prefix == other.key_prefix;
    }
};

enum class RedisValueKind {
    Status,
    String,
    Integer,
    Nil,
    Error,
    Array,
};

// hiredis reply 只在当次 command 内存活。这个递归值树复制所有
// 文本和子元素，上层不会持有 redisReply 的悬空指针。
struct RedisValue {
    RedisValueKind kind = RedisValueKind::Nil;
    std::string text;
    std::int64_t integer = 0;
    std::vector<RedisValue> elements;

    bool operator==(const RedisValue& other) const {
        return kind == other.kind &&
               text == other.text &&
               integer == other.integer &&
               elements == other.elements;
    }
};

enum class RedisCommandStatus {
    Ok,
    DeadlineExceeded,
    IoError,
    ProtocolError,
};

struct RedisCommandResult {
    RedisCommandStatus status = RedisCommandStatus::IoError;
    RedisValue value;
};

class IRedisCommandExecutor {
public:
    virtual ~IRedisCommandExecutor() = default;
    [[nodiscard]] virtual RedisCommandResult command(
        base::ArrayView<const std::string> argv,
        RedisDeadline deadline
    ) = 0;
    // WATCH/MULTI 后若无法确认 UNWATCH/DISCARD，唯一安全的
    // 处理是丢弃连接，避免后续命令继承未完成的事务状态。
    virtual void invalidate() noexcept = 0;
};

class RedisConnection final : public IRedisCommandExecutor {
public:
    [[nodiscard]] static std::unique_ptr<RedisConnection> connect(
        const RedisConfig& config,
        RedisDeadline deadline
    );

    ~RedisConnection() override;
    RedisConnection(RedisConnection&&) noexcept;
    RedisConnection& operator=(RedisConnection&&) noexcept;

    RedisConnection(const RedisConnection&) = delete;
    RedisConnection& operator=(const RedisConnection&) = delete;

    [[nodiscard]] RedisCommandResult command(
        base::ArrayView<const std::string> argv,
        RedisDeadline deadline
    ) override;
    void invalidate() noexcept override;

private:
    class Impl;
    explicit RedisConnection(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // 命名空间 aegisflow::storage
