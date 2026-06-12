#include "aegisflow/storage/redis_client.hpp"

#include <utility>

#ifdef AEGISFLOW_USE_HIREDIS
#include <hiredis/hiredis.h>

#include <memory>
#include <string>
#include <sys/time.h>
#endif

namespace aegisflow::storage {

RedisClient::RedisClient() = default;

RedisClient::RedisClient(RedisConfig config)
    : config_(std::move(config)) {}

RedisClient::~RedisClient() {
    close();
}

bool RedisClient::connect() {
    return connect(config_);
}

bool RedisClient::connect(const RedisConfig& config) {
    close();

    config_ = config;
    last_error_.clear();

#ifdef AEGISFLOW_USE_HIREDIS
    timeval timeout{};
    timeout.tv_sec = static_cast<time_t>(config_.timeout_ms / 1000);
    timeout.tv_usec = static_cast<suseconds_t>((config_.timeout_ms % 1000) * 1000);

    redisContext* context = redisConnectWithTimeout(
        config_.host.c_str(),
        config_.port,
        timeout
    );

    if (context == nullptr) {
        markUnavailable("redisConnectWithTimeout failed");
        return false;
    }

    if (context->err != 0) {
        const std::string error = context->errstr;
        redisFree(context);
        markUnavailable("redis connect failed: " + error);
        return false;
    }

    redisSetTimeout(context, timeout);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        conn_ = context;
        available_.store(true);
    }

    if (!config_.password.empty()) {
        using ReplyPtr = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;
        ReplyPtr reply(
            static_cast<redisReply*>(
                redisCommand(context, "AUTH %b", config_.password.data(), config_.password.size())
            ),
            freeReplyObject
        );

        if (!reply ||
            reply->type == REDIS_REPLY_ERROR ||
            reply->type != REDIS_REPLY_STATUS ||
            std::string(reply->str, reply->len) != "OK") {
            redisFree(context);
            markUnavailable("redis AUTH failed");
            return false;
        }
    }

    if (!ping()) {
        close();
        return false;
    }

    return true;
#else
    markUnavailable("hiredis support is disabled");
    return false;
#endif
}

void RedisClient::close() {
    std::lock_guard<std::mutex> lock(mutex_);

#ifdef AEGISFLOW_USE_HIREDIS
    if (conn_ != nullptr) {
        redisFree(conn_);
        conn_ = nullptr;
    }
#else
    conn_ = nullptr;
#endif

    available_.store(false);
}

std::optional<std::string> RedisClient::get(const std::string& key) {
    if (!available()) {
        return std::nullopt;
    }

#ifdef AEGISFLOW_USE_HIREDIS
    std::lock_guard<std::mutex> lock(mutex_);
    if (conn_ == nullptr) {
        markUnavailable("redis connection is not open");
        return std::nullopt;
    }

    using ReplyPtr = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;
    ReplyPtr reply(
        static_cast<redisReply*>(
            redisCommand(conn_, "GET %b", key.data(), key.size())
        ),
        freeReplyObject
    );

    if (!reply) {
        markUnavailable("redis GET failed");
        return std::nullopt;
    }

    if (reply->type == REDIS_REPLY_NIL) {
        return std::nullopt;
    }

    if (reply->type != REDIS_REPLY_STRING) {
        return std::nullopt;
    }

    return std::string(reply->str, reply->len);
#else
    (void)key;
    return std::nullopt;
#endif
}

bool RedisClient::setex(
    const std::string& key,
    int ttl_sec,
    const std::string& value
) {
    if (ttl_sec <= 0 || !available()) {
        return false;
    }

#ifdef AEGISFLOW_USE_HIREDIS
    std::lock_guard<std::mutex> lock(mutex_);
    if (conn_ == nullptr) {
        markUnavailable("redis connection is not open");
        return false;
    }

    using ReplyPtr = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;
    ReplyPtr reply(
        static_cast<redisReply*>(
            redisCommand(
                conn_,
                "SETEX %b %d %b",
                key.data(),
                key.size(),
                ttl_sec,
                value.data(),
                value.size()
            )
        ),
        freeReplyObject
    );

    if (!reply) {
        markUnavailable("redis SETEX failed");
        return false;
    }

    return reply->type == REDIS_REPLY_STATUS &&
        std::string(reply->str, reply->len) == "OK";
#else
    (void)key;
    (void)value;
    return false;
#endif
}

bool RedisClient::ping() {
#ifdef AEGISFLOW_USE_HIREDIS
    std::lock_guard<std::mutex> lock(mutex_);
    if (conn_ == nullptr) {
        markUnavailable("redis connection is not open");
        return false;
    }

    using ReplyPtr = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;
    ReplyPtr reply(
        static_cast<redisReply*>(redisCommand(conn_, "PING")),
        freeReplyObject
    );

    if (!reply) {
        markUnavailable("redis PING failed");
        return false;
    }

    const bool ok = reply->type == REDIS_REPLY_STATUS &&
        std::string(reply->str, reply->len) == "PONG";

    available_.store(ok);
    if (!ok) {
        last_error_ = "redis PING returned unexpected response";
    }

    return ok;
#else
    markUnavailable("hiredis support is disabled");
    return false;
#endif
}

bool RedisClient::available() const {
    return available_.load();
}

const std::string& RedisClient::lastError() const {
    return last_error_;
}

void RedisClient::markUnavailable(const std::string& error) {
    last_error_ = error;
    available_.store(false);
}

} // namespace aegisflow::storage