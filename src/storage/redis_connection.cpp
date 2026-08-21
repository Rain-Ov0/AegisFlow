#include "aegisflow/storage/redis_connection.hpp"

#include <hiredis/hiredis.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aegisflow::storage {
namespace {

struct RedisContextDeleter {
    void operator()(redisContext* context) const noexcept {
        if (context != nullptr) {
            redisFree(context);
        }
    }
};

struct RedisReplyDeleter {
    void operator()(redisReply* reply) const noexcept {
        if (reply != nullptr) {
            freeReplyObject(reply);
        }
    }
};

using ContextPtr = std::unique_ptr<redisContext, RedisContextDeleter>;
using ReplyPtr = std::unique_ptr<redisReply, RedisReplyDeleter>;

std::optional<std::chrono::microseconds> remainingTimeout(
    const std::chrono::milliseconds configured,
    const RedisDeadline deadline
) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline || configured.count() <= 0) {
        return std::nullopt;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
        deadline - now);
    const auto configured_us = std::chrono::duration_cast<
        std::chrono::microseconds>(configured);
    return std::max(
        std::chrono::microseconds(1),
        std::min(remaining, configured_us)
    );
}

timeval toTimeval(const std::chrono::microseconds timeout) noexcept {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        timeout);
    const auto microseconds = timeout - seconds;
    timeval result{};
    result.tv_sec =
        static_cast<decltype(timeval::tv_sec)>(seconds.count());
    result.tv_usec = static_cast<decltype(timeval::tv_usec)>(
        microseconds.count());
    return result;
}

RedisCommandResult commandResult(const RedisCommandStatus status) {
    RedisCommandResult result;
    result.status = status;
    return result;
}

RedisCommandResult successfulCommand(RedisValue value) {
    RedisCommandResult result;
    result.status = RedisCommandStatus::Ok;
    result.value = std::move(value);
    return result;
}

std::optional<RedisValue> copyReply(const redisReply& source) {
    RedisValue value;
    switch (source.type) {
    case REDIS_REPLY_STATUS:
        if (source.str == nullptr && source.len != 0) {
            return std::nullopt;
        }
        value.kind = RedisValueKind::Status;
        value.text.assign(source.str == nullptr ? "" : source.str, source.len);
        return value;
    case REDIS_REPLY_STRING:
        if (source.str == nullptr && source.len != 0) {
            return std::nullopt;
        }
        value.kind = RedisValueKind::String;
        value.text.assign(source.str == nullptr ? "" : source.str, source.len);
        return value;
    case REDIS_REPLY_INTEGER:
        value.kind = RedisValueKind::Integer;
        value.integer = source.integer;
        return value;
    case REDIS_REPLY_NIL:
        value.kind = RedisValueKind::Nil;
        return value;
    case REDIS_REPLY_ERROR:
        if (source.str == nullptr && source.len != 0) {
            return std::nullopt;
        }
        value.kind = RedisValueKind::Error;
        value.text.assign(source.str == nullptr ? "" : source.str, source.len);
        return value;
    case REDIS_REPLY_ARRAY:
        value.kind = RedisValueKind::Array;
        value.elements.reserve(source.elements);
        for (std::size_t index = 0; index < source.elements; ++index) {
            if (source.element[index] == nullptr) {
                return std::nullopt;
            }
            auto child = copyReply(*source.element[index]);
            if (!child.has_value()) {
                return std::nullopt;
            }
            value.elements.push_back(std::move(*child));
        }
        return value;
    default:
        return std::nullopt;
    }
}

bool isOk(const RedisCommandResult& result) noexcept {
    return result.status == RedisCommandStatus::Ok &&
           result.value.kind == RedisValueKind::Status &&
           result.value.text == "OK";
}

}  // 命名空间

class RedisConnection::Impl final {
public:
    Impl(ContextPtr context, const std::chrono::milliseconds command_timeout)
        : context_(std::move(context)),
          command_timeout_(command_timeout) {}

    RedisCommandResult command(
        const base::ArrayView<const std::string> argv,
        const RedisDeadline deadline
    ) {
        if (context_ == nullptr || argv.empty() || argv.size() > INT_MAX) {
            return commandResult(RedisCommandStatus::IoError);
        }
        const auto timeout = remainingTimeout(command_timeout_, deadline);
        if (!timeout.has_value()) {
            return commandResult(RedisCommandStatus::DeadlineExceeded);
        }
        if (redisSetTimeout(context_.get(), toTimeval(*timeout)) != REDIS_OK) {
            context_.reset();
            return commandResult(RedisCommandStatus::IoError);
        }

        std::vector<const char*> values;
        std::vector<std::size_t> lengths;
        values.reserve(argv.size());
        lengths.reserve(argv.size());
        for (const auto& argument : argv) {
            values.push_back(argument.data());
            lengths.push_back(argument.size());
        }

        ReplyPtr reply(static_cast<redisReply*>(redisCommandArgv(
            context_.get(),
            static_cast<int>(values.size()),
            values.data(),
            lengths.data()
        )));
        if (reply == nullptr) {
            context_.reset();
            return commandResult(RedisCommandStatus::IoError);
        }
        auto copied = copyReply(*reply);
        if (!copied.has_value()) {
            context_.reset();
            return commandResult(RedisCommandStatus::ProtocolError);
        }
        return successfulCommand(std::move(*copied));
    }

    void invalidate() noexcept { context_.reset(); }

private:
    ContextPtr context_;
    std::chrono::milliseconds command_timeout_;
};

std::unique_ptr<RedisConnection> RedisConnection::connect(
    const RedisConfig& config,
    const RedisDeadline deadline
) {
    if (config.host.empty() || config.port == 0 ||
        config.connect_timeout.count() <= 0 ||
        config.command_timeout.count() <= 0 ||
        config.database > 15 ||
        (!config.username.empty() && config.password.empty())) {
        return nullptr;
    }
    const auto connect_timeout = remainingTimeout(
        config.connect_timeout, deadline);
    if (!connect_timeout.has_value()) {
        return nullptr;
    }
    const auto command_timeout = remainingTimeout(
        config.command_timeout, deadline);
    if (!command_timeout.has_value()) {
        return nullptr;
    }

    const timeval connect_tv = toTimeval(*connect_timeout);
    const timeval command_tv = toTimeval(*command_timeout);
    redisOptions options{};
    REDIS_OPTIONS_SET_TCP(&options, config.host.c_str(), config.port);
    options.connect_timeout = &connect_tv;
    options.command_timeout = &command_tv;
    ContextPtr context(redisConnectWithOptions(&options));
    if (context == nullptr || context->err != 0) {
        return nullptr;
    }

    auto connection = std::unique_ptr<RedisConnection>(
        new RedisConnection(std::make_unique<Impl>(
            std::move(context), config.command_timeout)));
    if (!config.password.empty()) {
        const std::vector<std::string> auth = config.username.empty()
            ? std::vector<std::string>{"AUTH", config.password}
            : std::vector<std::string>{
                  "AUTH", config.username, config.password};
        if (!isOk(connection->command(auth, deadline))) {
            return nullptr;
        }
    }
    if (config.database != 0) {
        const std::vector<std::string> select{
            "SELECT", std::to_string(config.database)};
        if (!isOk(connection->command(select, deadline))) {
            return nullptr;
        }
    }
    return connection;
}

RedisConnection::RedisConnection(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

RedisConnection::~RedisConnection() = default;
RedisConnection::RedisConnection(RedisConnection&&) noexcept = default;
RedisConnection& RedisConnection::operator=(RedisConnection&&) noexcept =
    default;

RedisCommandResult RedisConnection::command(
    const base::ArrayView<const std::string> argv,
    const RedisDeadline deadline
) {
    return impl_->command(argv, deadline);
}

void RedisConnection::invalidate() noexcept {
    if (impl_ != nullptr) {
        impl_->invalidate();
    }
}

}  // 命名空间 aegisflow::storage
