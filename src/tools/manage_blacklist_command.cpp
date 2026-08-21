#include "aegisflow/tools/manage_blacklist_command.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace aegisflow::tools {
namespace {

using namespace std::chrono_literals;

struct ParsedOptions {
    std::optional<std::string> type;
    std::optional<std::string> id;
    std::optional<std::string> reason;
    std::optional<std::string> expire_at_ms;
    std::optional<std::string> confirm;
    bool all = false;
    bool wait = false;
};

ManageBlacklistParseResult usageError(std::string message) {
    ManageBlacklistParseResult result;
    result.command = std::nullopt;
    result.error = std::move(message);
    return result;
}

ManageBlacklistParseResult parsedCommand(ManageBlacklistCommand command) {
    ManageBlacklistParseResult result;
    result.command = std::move(command);
    return result;
}

bool assignOnce(
    std::optional<std::string>& destination,
    const std::string_view value
) {
    if (destination.has_value()) {
        return false;
    }
    destination = std::string(value);
    return true;
}

std::optional<risk::EntityType> parseEntityType(const std::string_view value) {
    if (value == "user") {
        return risk::EntityType::User;
    }
    if (value == "ip") {
        return risk::EntityType::Ip;
    }
    if (value == "device") {
        return risk::EntityType::Device;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> parseUnsigned(const std::string_view value) {
    if (value.empty() || value.front() == '-') {
        return std::nullopt;
    }
    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

ManageBlacklistParseResult parseOptions(
    const ManageBlacklistAction action,
    const std::string& config_path,
    const base::ArrayView<const std::string_view> arguments
) {
    ParsedOptions options;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto option = arguments[index];
        const auto readValue = [&](std::optional<std::string>& destination)
            -> std::optional<std::string> {
            if (index + 1 >= arguments.size()) {
                return "参数 " + std::string(option) + " 缺少值";
            }
            ++index;
            if (!assignOnce(destination, arguments[index])) {
                return "参数 " + std::string(option) + " 不能重复";
            }
            return std::nullopt;
        };

        std::optional<std::string> error;
        if (option == "--type") {
            error = readValue(options.type);
        } else if (option == "--id") {
            error = readValue(options.id);
        } else if (option == "--reason") {
            error = readValue(options.reason);
        } else if (option == "--expire-at-ms") {
            error = readValue(options.expire_at_ms);
        } else if (option == "--confirm") {
            error = readValue(options.confirm);
        } else if (option == "--all") {
            if (options.all) {
                error = "参数 --all 不能重复";
            }
            options.all = true;
        } else if (option == "--wait") {
            if (options.wait) {
                error = "参数 --wait 不能重复";
            }
            options.wait = true;
        } else {
            return usageError("未知参数: " + std::string(option));
        }
        if (error.has_value()) {
            return usageError(std::move(*error));
        }
    }

    ManageBlacklistCommand command;
    command.config_path = config_path;
    command.action = action;

    if (action == ManageBlacklistAction::Add) {
        if (!options.type.has_value() || !options.id.has_value() ||
            !options.reason.has_value()) {
            return usageError("add 必须同时提供 --type、--id 和 --reason");
        }
        if (options.all || options.wait || options.confirm.has_value()) {
            return usageError("add 不接受 --all、--wait 或 --confirm");
        }
        const auto type = parseEntityType(*options.type);
        if (!type.has_value()) {
            return usageError("--type 必须是 user、ip 或 device");
        }
        std::uint64_t expire_at_ms = 0;
        if (options.expire_at_ms.has_value()) {
            const auto parsed = parseUnsigned(*options.expire_at_ms);
            if (!parsed.has_value()) {
                return usageError("--expire-at-ms 必须是无符号整数");
            }
            expire_at_ms = *parsed;
        }
        command.mutation = risk::BlacklistMutation::upsert(
            *type, *options.id, *options.reason, expire_at_ms);
        if (!command.mutation.has_value()) {
            return usageError("add 的实体、reason 或过期时间无效");
        }
        return parsedCommand(std::move(command));
    }

    if (action == ManageBlacklistAction::Disable) {
        if (!options.type.has_value() || !options.id.has_value()) {
            return usageError("disable 必须同时提供 --type 和 --id");
        }
        if (options.reason.has_value() || options.expire_at_ms.has_value() ||
            options.confirm.has_value() || options.all || options.wait) {
            return usageError("disable 只接受 --type 和 --id");
        }
        const auto type = parseEntityType(*options.type);
        if (!type.has_value()) {
            return usageError("--type 必须是 user、ip 或 device");
        }
        command.mutation = risk::BlacklistMutation::disable(
            *type, *options.id);
        if (!command.mutation.has_value()) {
            return usageError("disable 的实体无效");
        }
        return parsedCommand(std::move(command));
    }

    if (!options.all || options.confirm != "benchmark-reset") {
        return usageError(
            "clear 必须提供 --all --confirm benchmark-reset");
    }
    if (options.type.has_value() || options.id.has_value() ||
        options.reason.has_value() || options.expire_at_ms.has_value()) {
        return usageError("clear 不接受实体或 reason 参数");
    }
    command.wait = options.wait;
    command.mutation = risk::BlacklistMutation::clearAll();
    return parsedCommand(std::move(command));
}

std::string_view actionName(const ManageBlacklistAction action) {
    switch (action) {
    case ManageBlacklistAction::Add:
        return "UPSERT";
    case ManageBlacklistAction::Disable:
        return "DISABLE";
    case ManageBlacklistAction::ClearAll:
        return "CLEAR_ALL";
    }
    return "UNKNOWN";
}

std::string_view statusName(const storage::StoreStatus status) {
    switch (status) {
    case storage::StoreStatus::Ok:
        return "ok";
    case storage::StoreStatus::WatchConflict:
        return "watch_conflict";
    case storage::StoreStatus::NotReady:
        return "not_ready";
    case storage::StoreStatus::InvalidRemoteState:
        return "invalid_remote_state";
    case storage::StoreStatus::DeadlineExceeded:
        return "deadline_exceeded";
    case storage::StoreStatus::IoError:
        return "io_error";
    case storage::StoreStatus::CommitUnknown:
        return "commit_unknown";
    case storage::StoreStatus::TransactionError:
        return "transaction_error";
    case storage::StoreStatus::ProtocolError:
        return "protocol_error";
    }
    return "unknown";
}

bool sleepForPoll(
    IManageBlacklistClock& clock,
    const storage::RedisDeadline deadline
) {
    const auto now = clock.now();
    if (now >= deadline) {
        return false;
    }
    clock.sleepUntil(std::min(deadline, now + 25ms));
    return clock.now() < deadline;
}

storage::StoreResult applyWithRetry(
    IManageBlacklistBackend& backend,
    const risk::BlacklistMutation& mutation,
    IManageBlacklistClock& clock,
    const storage::RedisDeadline deadline
) {
    while (clock.now() < deadline) {
        auto result = backend.applyMutation(mutation, deadline);
        if (result.status != storage::StoreStatus::WatchConflict) {
            return result;
        }
        if (!sleepForPoll(clock, deadline)) {
            break;
        }
    }
    storage::StoreResult result;
    result.status = storage::StoreStatus::DeadlineExceeded;
    result.revision = std::nullopt;
    return result;
}

}  // namespace

ManageBlacklistParseResult parseManageBlacklistCommand(
    const base::ArrayView<const std::string_view> arguments
) {
    if (arguments.empty()) {
        return usageError("缺少 add、disable 或 clear 操作");
    }

    std::size_t index = 0;
    std::string config_path = "config/server.conf";
    if (arguments[index] == "--config") {
        if (index + 1 >= arguments.size()) {
            return usageError("参数 --config 缺少路径");
        }
        config_path = std::string(arguments[index + 1]);
        if (config_path.empty() || config_path.find('\0') != std::string::npos) {
            return usageError("配置路径不能为空");
        }
        index += 2;
    }
    if (index >= arguments.size()) {
        return usageError("缺少 add、disable 或 clear 操作");
    }

    ManageBlacklistAction action;
    if (arguments[index] == "add") {
        action = ManageBlacklistAction::Add;
    } else if (arguments[index] == "disable") {
        action = ManageBlacklistAction::Disable;
    } else if (arguments[index] == "clear") {
        action = ManageBlacklistAction::ClearAll;
    } else {
        return usageError("未知操作: " + std::string(arguments[index]));
    }
    ++index;
    return parseOptions(action, config_path, arguments.subview(index));
}

bool resetConverged(
    const storage::ResetConvergence& redis,
    const MysqlBlacklistCountResult& mysql
) noexcept {
    return redis.redisAndPublicationEmpty() && mysql.success &&
           mysql.counts.empty();
}

int runManageBlacklistCommand(
    const base::ArrayView<const std::string_view> arguments,
    IManageBlacklistBackendFactory& factory,
    IManageBlacklistClock& clock,
    std::ostream& output,
    std::ostream& error
) {
    const auto parsed = parseManageBlacklistCommand(arguments);
    if (!parsed.command.has_value()) {
        error << "manage_blacklist: " << parsed.error << '\n';
        return 2;
    }

    // 起点必须早于配置读取与依赖连接，否则慢连接会被
    // 排除在整条管理命令的 reset timeout 之外。
    const auto started_at = clock.now();
    auto opened = factory.open(*parsed.command, started_at);
    if (!opened.backend || opened.timeout <= 0ms) {
        error << "manage_blacklist: "
              << (opened.error.empty() ? "无法打开依赖" : opened.error)
              << '\n';
        return 1;
    }

    const auto maximum = storage::RedisDeadline::max() - started_at;
    const auto timeout = std::min(
        std::chrono::duration_cast<std::chrono::milliseconds>(maximum),
        opened.timeout);
    const auto deadline = started_at + timeout;
    if (clock.now() >= deadline) {
        error << "manage_blacklist: deadline_exceeded while opening dependencies\n";
        return 1;
    }
    auto& backend = *opened.backend;
    const auto& command = *parsed.command;

    if (command.action != ManageBlacklistAction::ClearAll) {
        const auto applied = applyWithRetry(
            backend, *command.mutation, clock, deadline);
        if (applied.status != storage::StoreStatus::Ok ||
            !applied.revision.has_value()) {
            const auto reported =
                applied.status == storage::StoreStatus::Ok
                    ? storage::StoreStatus::ProtocolError
                    : applied.status;
            error << "manage_blacklist: operation=" << actionName(command.action)
                  << " status=" << statusName(reported) << '\n';
            return 1;
        }
        output << "status=ok operation=" << actionName(command.action)
               << " revision=" << *applied.revision << '\n';
        return 0;
    }

    const auto requested = backend.requestResetBarrier(deadline);
    if (requested.status != storage::StoreStatus::Ok ||
        !requested.revision.has_value() || *requested.revision == 0) {
        const auto reported =
            requested.status == storage::StoreStatus::Ok
                ? storage::StoreStatus::ProtocolError
                : requested.status;
        error << "manage_blacklist: operation=CLEAR_ALL barrier="
              << statusName(reported) << '\n';
        return 1;
    }
    const auto token = *requested.revision;

    bool acknowledged = false;
    while (clock.now() < deadline) {
        const auto barrier = backend.readResetBarrier(deadline);
        if (barrier.status != storage::StoreStatus::Ok) {
            error << "manage_blacklist: operation=CLEAR_ALL barrier="
                  << statusName(barrier.status) << '\n';
            return 1;
        }
        if (barrier.state.completed >= token) {
            acknowledged = true;
            break;
        }
        if (!sleepForPoll(clock, deadline)) {
            break;
        }
    }
    if (!acknowledged) {
        error << "manage_blacklist: operation=CLEAR_ALL barrier=timeout token="
              << token << '\n';
        return 1;
    }

    const auto applied = applyWithRetry(
        backend, *command.mutation, clock, deadline);
    if (applied.status != storage::StoreStatus::Ok ||
        !applied.revision.has_value()) {
        const auto reported =
            applied.status == storage::StoreStatus::Ok
                ? storage::StoreStatus::ProtocolError
                : applied.status;
        error << "manage_blacklist: operation=CLEAR_ALL status="
              << statusName(reported) << '\n';
        return 1;
    }

    if (command.wait) {
        bool converged = false;
        while (clock.now() < deadline) {
            const auto redis = backend.readResetConvergence(deadline);
            if (redis.status != storage::StoreStatus::Ok) {
                error << "manage_blacklist: operation=CLEAR_ALL wait="
                      << statusName(redis.status) << '\n';
                return 1;
            }
            const auto mysql = backend.countEnabledBlacklists(deadline);
            if (!mysql.success) {
                error << "manage_blacklist: operation=CLEAR_ALL wait="
                      << (clock.now() >= deadline
                              ? "deadline_exceeded"
                              : "mysql_error")
                      << '\n';
                return 1;
            }
            if (resetConverged(redis, mysql)) {
                converged = true;
                break;
            }
            if (!sleepForPoll(clock, deadline)) {
                break;
            }
        }
        if (!converged) {
            error << "manage_blacklist: operation=CLEAR_ALL wait=timeout token="
                  << token << '\n';
            return 1;
        }
    }

    output << "status=ok operation=CLEAR_ALL revision=" << *applied.revision
           << " barrier_token=" << token
           << " converged=" << (command.wait ? 1 : 0) << '\n';
    return 0;
}

}  // namespace aegisflow::tools
