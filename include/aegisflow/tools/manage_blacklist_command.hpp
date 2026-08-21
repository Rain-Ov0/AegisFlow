#pragma once

#include "aegisflow/base/array_view.hpp"
#include "aegisflow/risk/blacklist_mutation.hpp"
#include "aegisflow/storage/blacklist_redis_store.hpp"
#include "aegisflow/storage/mysql_dao.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace aegisflow::tools {

enum class ManageBlacklistAction {
    Add,
    Disable,
    ClearAll,
};

struct ManageBlacklistCommand {
    std::string config_path = "config/server.conf";
    ManageBlacklistAction action = ManageBlacklistAction::Add;
    std::optional<risk::BlacklistMutation> mutation;
    bool wait = false;
};

struct ManageBlacklistParseResult {
    std::optional<ManageBlacklistCommand> command;
    std::string error;
};

// 解析层完成参数唯一性、操作间互斥与领域值校验。
// 特别是 clear 的确认字符串在打开 Redis 连接前就必须通过。
[[nodiscard]] ManageBlacklistParseResult parseManageBlacklistCommand(
    base::ArrayView<const std::string_view> arguments
);

struct MysqlBlacklistCountResult {
    bool success = false;
    storage::BlacklistCounts counts;
};

class IManageBlacklistBackend {
public:
    virtual ~IManageBlacklistBackend() = default;

    [[nodiscard]] virtual storage::StoreResult applyMutation(
        const risk::BlacklistMutation& mutation,
        storage::RedisDeadline deadline
    ) = 0;
    [[nodiscard]] virtual storage::StoreResult requestResetBarrier(
        storage::RedisDeadline deadline
    ) = 0;
    [[nodiscard]] virtual storage::ResetReadResult readResetBarrier(
        storage::RedisDeadline deadline
    ) = 0;
    [[nodiscard]] virtual storage::ResetConvergence readResetConvergence(
        storage::RedisDeadline deadline
    ) = 0;
    [[nodiscard]] virtual MysqlBlacklistCountResult countEnabledBlacklists(
        storage::RedisDeadline deadline
    ) = 0;
};

struct ManageBlacklistOpenResult {
    std::unique_ptr<IManageBlacklistBackend> backend;
    std::chrono::milliseconds timeout{0};
    std::string error;
};

class IManageBlacklistBackendFactory {
public:
    virtual ~IManageBlacklistBackendFactory() = default;
    [[nodiscard]] virtual ManageBlacklistOpenResult open(
        const ManageBlacklistCommand& command,
        storage::RedisDeadline started_at
    ) = 0;
};

class IManageBlacklistClock {
public:
    virtual ~IManageBlacklistClock() = default;
    [[nodiscard]] virtual storage::RedisDeadline now() const = 0;
    virtual void sleepUntil(storage::RedisDeadline deadline) = 0;
};

// 将命令解析、依赖打开与有界轮询串在同一可测边界中。
// 返回值遵循命令行惯例：0 成功，1 运行期失败，2 用法错误。
int runManageBlacklistCommand(
    base::ArrayView<const std::string_view> arguments,
    IManageBlacklistBackendFactory& factory,
    IManageBlacklistClock& clock,
    std::ostream& output,
    std::ostream& error
);

[[nodiscard]] bool resetConverged(
    const storage::ResetConvergence& redis,
    const MysqlBlacklistCountResult& mysql
) noexcept;

}  // namespace aegisflow::tools
