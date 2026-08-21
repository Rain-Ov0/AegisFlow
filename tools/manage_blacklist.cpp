#include "aegisflow/config/config.hpp"
#include "aegisflow/tools/manage_blacklist_command.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

class SteadyClock final : public aegisflow::tools::IManageBlacklistClock {
public:
    aegisflow::storage::RedisDeadline now() const override {
        return std::chrono::steady_clock::now();
    }

    void sleepUntil(
        const aegisflow::storage::RedisDeadline deadline
    ) override {
        std::this_thread::sleep_until(deadline);
    }
};

aegisflow::tools::ManageBlacklistOpenResult openFailure(
    const std::chrono::milliseconds timeout,
    std::string error
) {
    aegisflow::tools::ManageBlacklistOpenResult result;
    result.timeout = timeout;
    result.error = std::move(error);
    return result;
}

aegisflow::tools::ManageBlacklistOpenResult openSuccess(
    std::unique_ptr<aegisflow::tools::IManageBlacklistBackend> backend,
    const std::chrono::milliseconds timeout
) {
    aegisflow::tools::ManageBlacklistOpenResult result;
    result.backend = std::move(backend);
    result.timeout = timeout;
    return result;
}

class ManageBlacklistBackend final
    : public aegisflow::tools::IManageBlacklistBackend {
public:
    ManageBlacklistBackend(
        std::unique_ptr<aegisflow::storage::RedisConnection> redis,
        aegisflow::storage::RedisKeySet keys,
        std::unique_ptr<aegisflow::storage::MysqlDao> mysql
    )
        : redis_(std::move(redis)),
          store_(*redis_, std::move(keys)),
          mysql_(std::move(mysql)) {}

    aegisflow::storage::StoreResult applyMutation(
        const aegisflow::risk::BlacklistMutation& mutation,
        const aegisflow::storage::RedisDeadline deadline
    ) override {
        return store_.applyMutations(
            aegisflow::base::ArrayView<
                const aegisflow::risk::BlacklistMutation>(&mutation, 1),
            deadline);
    }

    aegisflow::storage::StoreResult requestResetBarrier(
        const aegisflow::storage::RedisDeadline deadline
    ) override {
        return store_.requestResetBarrier(deadline);
    }

    aegisflow::storage::ResetReadResult readResetBarrier(
        const aegisflow::storage::RedisDeadline deadline
    ) override {
        return store_.readResetBarrier(deadline);
    }

    aegisflow::storage::ResetConvergence readResetConvergence(
        const aegisflow::storage::RedisDeadline deadline
    ) override {
        return store_.readResetConvergence(deadline);
    }

    aegisflow::tools::MysqlBlacklistCountResult
    countEnabledBlacklists(
        const aegisflow::storage::RedisDeadline deadline
    ) override {
        aegisflow::tools::MysqlBlacklistCountResult result;
        if (!mysql_) {
            return result;
        }
        result.counts = mysql_->countEnabledBlacklists(deadline);
        result.success = mysql_->lastBlacklistCountSucceeded();
        return result;
    }

private:
    // Store 借用 connection，因此 connection 声明在前并最后析构。
    std::unique_ptr<aegisflow::storage::RedisConnection> redis_;
    aegisflow::storage::BlacklistRedisStore store_;
    std::unique_ptr<aegisflow::storage::MysqlDao> mysql_;
};

class ManageBlacklistBackendFactory final
    : public aegisflow::tools::IManageBlacklistBackendFactory {
public:
    aegisflow::tools::ManageBlacklistOpenResult open(
        const aegisflow::tools::ManageBlacklistCommand& command,
        const aegisflow::storage::RedisDeadline started_at
    ) override {
        try {
            const auto config =
                aegisflow::config::loadAppConfig(command.config_path);
            const auto timeout = config.handler.blacklist_cache.reset_timeout;
            const auto maximum =
                aegisflow::storage::RedisDeadline::max() - started_at;
            const auto bounded_timeout = std::min(
                std::chrono::duration_cast<std::chrono::milliseconds>(maximum),
                timeout);
            const auto deadline = started_at + bounded_timeout;
            auto redis = aegisflow::storage::RedisConnection::connect(
                config.handler.redis, deadline);
            if (!redis) {
                return openFailure(timeout, "Redis 连接或初始化失败");
            }

            std::unique_ptr<aegisflow::storage::MysqlDao> mysql;
            if (command.action ==
                    aegisflow::tools::ManageBlacklistAction::ClearAll &&
                command.wait) {
                mysql = std::make_unique<aegisflow::storage::MysqlDao>(
                    config.handler.mysql);
                if (!mysql->connect(deadline)) {
                    return openFailure(
                        timeout, "MySQL 连接或 UTC 会话初始化失败");
                }
            }

            auto backend = std::make_unique<ManageBlacklistBackend>(
                std::move(redis),
                aegisflow::storage::RedisKeySet::fromPrefix(
                    config.handler.redis.key_prefix),
                std::move(mysql));
            return openSuccess(std::move(backend), timeout);
        } catch (const std::exception& exception) {
            return openFailure(
                std::chrono::milliseconds(0), exception.what());
        }
    }
};

}  // namespace

int main(const int argc, char* argv[]) {
    std::vector<std::string_view> arguments;
    arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    ManageBlacklistBackendFactory factory;
    SteadyClock clock;
    try {
        return aegisflow::tools::runManageBlacklistCommand(
            arguments, factory, clock, std::cout, std::cerr);
    } catch (const std::exception& exception) {
        std::cerr << "manage_blacklist: " << exception.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "manage_blacklist: 未知错误\n";
        return 1;
    }
}
