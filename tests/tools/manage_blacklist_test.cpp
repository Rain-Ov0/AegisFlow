#include "aegisflow/tools/manage_blacklist_command.hpp"
#include "tests/support/test_harness.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using aegisflow::risk::BlacklistMutation;
using aegisflow::risk::BlacklistMutationOperation;
using aegisflow::risk::EntityType;
using aegisflow::storage::ResetConvergence;
using aegisflow::storage::ResetReadResult;
using aegisflow::storage::StoreResult;
using aegisflow::storage::StoreStatus;
using aegisflow::test::require;
using aegisflow::tools::IManageBlacklistBackend;
using aegisflow::tools::IManageBlacklistBackendFactory;
using aegisflow::tools::IManageBlacklistClock;
using aegisflow::tools::ManageBlacklistAction;
using aegisflow::tools::ManageBlacklistCommand;
using aegisflow::tools::ManageBlacklistOpenResult;
using aegisflow::tools::MysqlBlacklistCountResult;
using namespace std::chrono_literals;

StoreResult storeResult(
    const StoreStatus status,
    std::optional<std::uint64_t> revision = std::nullopt
) {
    StoreResult result;
    result.status = status;
    result.revision = revision;
    return result;
}

ResetReadResult resetReadResult(
    const StoreStatus status,
    const std::uint64_t requested,
    const std::uint64_t completed
) {
    ResetReadResult result;
    result.status = status;
    result.state.requested = requested;
    result.state.completed = completed;
    return result;
}

ResetConvergence convergence(
    const std::uint64_t users,
    const std::uint64_t pending,
    const std::uint64_t revision,
    const std::uint64_t published
) {
    ResetConvergence result;
    result.status = StoreStatus::Ok;
    result.user_count = users;
    result.pending_count = pending;
    result.revision = revision;
    result.published_revision = published;
    return result;
}

MysqlBlacklistCountResult mysqlCountResult(
    const bool success = true,
    const std::uint64_t users = 0,
    const std::uint64_t ips = 0,
    const std::uint64_t devices = 0
) {
    MysqlBlacklistCountResult result;
    result.success = success;
    result.counts.users = users;
    result.counts.ips = ips;
    result.counts.devices = devices;
    return result;
}

ManageBlacklistOpenResult failedOpenResult(
    const std::chrono::milliseconds timeout,
    std::string error
) {
    ManageBlacklistOpenResult result;
    result.timeout = timeout;
    result.error = std::move(error);
    return result;
}

ManageBlacklistOpenResult successfulOpenResult(
    std::unique_ptr<IManageBlacklistBackend> backend,
    const std::chrono::milliseconds timeout
) {
    ManageBlacklistOpenResult result;
    result.backend = std::move(backend);
    result.timeout = timeout;
    return result;
}

struct BackendState {
    std::vector<StoreResult> apply_results;
    std::vector<ResetReadResult> barriers;
    std::vector<ResetConvergence> convergence;
    std::vector<MysqlBlacklistCountResult> mysql;
    StoreResult request_result;
    std::vector<BlacklistMutation> applied;
    std::vector<aegisflow::storage::RedisDeadline> mysql_deadlines;
    std::size_t apply_index = 0;
    std::size_t barrier_index = 0;
    std::size_t convergence_index = 0;
    std::size_t mysql_index = 0;
};

BackendState defaultBackendState() {
    BackendState state;
    state.apply_results = {storeResult(StoreStatus::Ok, 2)};
    state.barriers = {resetReadResult(StoreStatus::Ok, 1, 1)};
    state.convergence = {convergence(0, 0, 2, 2)};
    state.mysql = {mysqlCountResult()};
    state.request_result = storeResult(StoreStatus::Ok, 1);
    return state;
}

template <typename T>
const T& scriptedValue(const std::vector<T>& values, std::size_t& index) {
    require(!values.empty(), "script 不能为空");
    const auto selected = std::min(index, values.size() - 1);
    ++index;
    return values[selected];
}

class ScriptedBackend final : public IManageBlacklistBackend {
public:
    explicit ScriptedBackend(std::shared_ptr<BackendState> state)
        : state_(std::move(state)) {}

    StoreResult applyMutation(
        const BlacklistMutation& mutation,
        aegisflow::storage::RedisDeadline
    ) override {
        state_->applied.push_back(mutation);
        return scriptedValue(state_->apply_results, state_->apply_index);
    }

    StoreResult requestResetBarrier(
        aegisflow::storage::RedisDeadline
    ) override {
        return state_->request_result;
    }

    ResetReadResult readResetBarrier(
        aegisflow::storage::RedisDeadline
    ) override {
        return scriptedValue(state_->barriers, state_->barrier_index);
    }

    ResetConvergence readResetConvergence(
        aegisflow::storage::RedisDeadline
    ) override {
        return scriptedValue(
            state_->convergence, state_->convergence_index);
    }

    MysqlBlacklistCountResult countEnabledBlacklists(
        const aegisflow::storage::RedisDeadline deadline
    ) override {
        state_->mysql_deadlines.push_back(deadline);
        return scriptedValue(state_->mysql, state_->mysql_index);
    }

private:
    std::shared_ptr<BackendState> state_;
};

class ScriptedFactory final : public IManageBlacklistBackendFactory {
public:
    std::shared_ptr<BackendState> state =
        std::make_shared<BackendState>(defaultBackendState());
    std::chrono::milliseconds timeout{200};
    std::string open_error;
    std::size_t open_count = 0;
    std::optional<ManageBlacklistCommand> opened_command;
    std::optional<aegisflow::storage::RedisDeadline> opened_at;
    std::function<void()> on_open;

    ManageBlacklistOpenResult open(
        const ManageBlacklistCommand& command,
        const aegisflow::storage::RedisDeadline started_at
    ) override {
        ++open_count;
        opened_command = command;
        opened_at = started_at;
        if (on_open) {
            on_open();
        }
        if (!open_error.empty()) {
            return failedOpenResult(timeout, open_error);
        }
        return successfulOpenResult(
            std::make_unique<ScriptedBackend>(state), timeout);
    }
};

class FakeClock final : public IManageBlacklistClock {
public:
    aegisflow::storage::RedisDeadline now() const override { return now_; }

    void sleepUntil(const aegisflow::storage::RedisDeadline deadline) override {
        require(deadline >= now_, "轮询时间不得倒退");
        now_ = deadline;
        ++sleep_count;
    }

    void advance(const std::chrono::milliseconds elapsed) {
        now_ += elapsed;
    }

    std::size_t sleep_count = 0;

private:
    aegisflow::storage::RedisDeadline now_{};
};

template <std::size_t Size>
int run(
    const std::array<std::string_view, Size>& arguments,
    ScriptedFactory& factory,
    FakeClock& clock,
    std::ostringstream& output,
    std::ostringstream& error
) {
    return aegisflow::tools::runManageBlacklistCommand(
        arguments, factory, clock, output, error);
}

void parserBuildsValidatedMutations() {
    const std::array add{
        std::string_view("--config"), std::string_view("custom.conf"),
        std::string_view("add"), std::string_view("--type"),
        std::string_view("ip"), std::string_view("--id"),
        std::string_view("2001:0db8::1"), std::string_view("--reason"),
        std::string_view("manual"), std::string_view("--expire-at-ms"),
        std::string_view("1234"),
    };
    const auto add_parsed =
        aegisflow::tools::parseManageBlacklistCommand(add);
    require(add_parsed.command.has_value(), "合法 add 应解析成功");
    require(add_parsed.command->config_path == "custom.conf", "config 路径");
    require(add_parsed.command->action == ManageBlacklistAction::Add, "add 动作");
    const auto& upsert = *add_parsed.command->mutation;
    require(upsert.operation() == BlacklistMutationOperation::Upsert, "upsert");
    require(upsert.entityType() == EntityType::Ip, "IP 类型");
    require(upsert.id() == "2001:db8::1", "IP 应在解析边界标准化");
    require(upsert.reason() == "manual", "reason");
    require(upsert.expireAtMs() == 1234, "expire_at_ms");

    const std::array disable{
        std::string_view("disable"), std::string_view("--type"),
        std::string_view("device"), std::string_view("--id"),
        std::string_view("device-1"),
    };
    const auto disabled = aegisflow::tools::parseManageBlacklistCommand(disable);
    require(disabled.command.has_value(), "合法 disable");
    require(disabled.command->mutation->operation() ==
                BlacklistMutationOperation::Disable,
            "disable 值对象");

    const std::array clear{
        std::string_view("clear"), std::string_view("--all"),
        std::string_view("--confirm"), std::string_view("benchmark-reset"),
        std::string_view("--wait"),
    };
    const auto cleared = aegisflow::tools::parseManageBlacklistCommand(clear);
    require(cleared.command.has_value() && cleared.command->wait, "clear --wait");
    require(cleared.command->mutation->operation() ==
                BlacklistMutationOperation::ClearAll,
            "clear-all 值对象");
}

void parserRejectsUnknownDuplicateAndMixedOptions() {
    const auto mustFail = [](const auto& arguments) {
        const auto parsed =
            aegisflow::tools::parseManageBlacklistCommand(arguments);
        require(!parsed.command.has_value() && !parsed.error.empty(),
                "非法命令应返回用法错误");
    };

    mustFail(std::array{
        std::string_view("add"), std::string_view("--type"),
        std::string_view("user"), std::string_view("--type"),
        std::string_view("ip"), std::string_view("--id"),
        std::string_view("u"), std::string_view("--reason"),
        std::string_view("r"),
    });
    mustFail(std::array{
        std::string_view("add"), std::string_view("--type"),
        std::string_view("user"), std::string_view("--id"),
        std::string_view("u"), std::string_view("--reason"),
        std::string_view("r"), std::string_view("--wait"),
    });
    mustFail(std::array{
        std::string_view("disable"), std::string_view("--type"),
        std::string_view("bogus"), std::string_view("--id"),
        std::string_view("u"),
    });
    mustFail(std::array{
        std::string_view("clear"), std::string_view("--all"),
        std::string_view("--confirm"), std::string_view("wrong"),
    });
    mustFail(std::array{
        std::string_view("clear"), std::string_view("--all"),
        std::string_view("--confirm"), std::string_view("benchmark-reset"),
        std::string_view("--mystery"),
    });
    mustFail(std::array{
        std::string_view("add"), std::string_view("--type"),
        std::string_view("ip"), std::string_view("--id"),
        std::string_view("not-an-ip"), std::string_view("--reason"),
        std::string_view("r"),
    });
}

void invalidClearNeverOpensBackend() {
    ScriptedFactory factory;
    FakeClock clock;
    std::ostringstream output;
    std::ostringstream error;
    const std::array arguments{
        std::string_view("clear"), std::string_view("--all"),
    };
    require(run(arguments, factory, clock, output, error) == 2,
            "缺失确认值应返回 2");
    require(factory.open_count == 0, "用法校验失败不得连接 Redis");
    require(output.str().empty(), "失败不应输出成功摘要");
}

void dependencyOpenConsumesTheCommandDeadline() {
    ScriptedFactory factory;
    factory.timeout = 200ms;
    FakeClock clock;
    factory.on_open = [&clock] { clock.advance(201ms); };
    std::ostringstream output;
    std::ostringstream error;
    const std::array arguments{
        std::string_view("disable"), std::string_view("--type"),
        std::string_view("user"), std::string_view("--id"),
        std::string_view("user-1"),
    };

    require(run(arguments, factory, clock, output, error) == 1,
            "依赖打开耗尽全局 timeout 后不得执行业务命令");
    require(factory.opened_at == aegisflow::storage::RedisDeadline{},
            "deadline 起点必须在 factory.open 前采样");
    require(factory.state->applied.empty(),
            "打开依赖后已超时不得写 Redis");
    require(error.str().find("deadline_exceeded") != std::string::npos,
            "应明确报告打开依赖阶段超时");
}

void mutationRetriesWatchConflict() {
    ScriptedFactory factory;
    factory.state->apply_results = {
        storeResult(StoreStatus::WatchConflict),
        storeResult(StoreStatus::Ok, 9),
    };
    FakeClock clock;
    std::ostringstream output;
    std::ostringstream error;
    const std::array arguments{
        std::string_view("add"), std::string_view("--type"),
        std::string_view("user"), std::string_view("--id"),
        std::string_view("user-1"), std::string_view("--reason"),
        std::string_view("manual"),
    };
    require(run(arguments, factory, clock, output, error) == 0,
            "WATCH 冲突后应整体重试");
    require(factory.state->applied.size() == 2, "应提交两次");
    require(clock.sleep_count == 1, "冲突重试间应让出时间");
    require(output.str() == "status=ok operation=UPSERT revision=9\n",
            "成功输出应是单行 key=value 摘要");
    require(error.str().empty(), "成功不应输出错误");
}

void clearCannotBypassBarrier() {
    ScriptedFactory factory;
    factory.timeout = 50ms;
    factory.state->barriers = {
        resetReadResult(StoreStatus::Ok, 1, 0),
    };
    FakeClock clock;
    std::ostringstream output;
    std::ostringstream error;
    const std::array arguments{
        std::string_view("clear"), std::string_view("--all"),
        std::string_view("--confirm"), std::string_view("benchmark-reset"),
    };
    require(run(arguments, factory, clock, output, error) == 1,
            "P4 无服务 ack 时 clear 应超时");
    require(factory.state->applied.empty(),
            "barrier 未 ack 不得执行 CLEAR_ALL 事务");
    require(error.str().find("barrier=timeout") != std::string::npos,
            "应明确报告 barrier 超时");
}

void clearWaitsForEveryConvergenceLayer() {
    ScriptedFactory factory;
    factory.timeout = 500ms;
    factory.state->barriers = {
        resetReadResult(StoreStatus::Ok, 1, 0),
        resetReadResult(StoreStatus::Ok, 1, 1),
    };
    factory.state->apply_results = {storeResult(StoreStatus::Ok, 4)};
    factory.state->convergence = {
        convergence(1, 0, 4, 4),
        convergence(0, 1, 4, 4),
        convergence(0, 0, 4, 3),
        convergence(0, 0, 4, 4),
        convergence(0, 0, 4, 4),
    };
    factory.state->mysql = {
        mysqlCountResult(),
        mysqlCountResult(),
        mysqlCountResult(),
        mysqlCountResult(true, 1),
        mysqlCountResult(),
    };

    FakeClock clock;
    std::ostringstream output;
    std::ostringstream error;
    const std::array arguments{
        std::string_view("clear"), std::string_view("--all"),
        std::string_view("--confirm"), std::string_view("benchmark-reset"),
        std::string_view("--wait"),
    };
    require(run(arguments, factory, clock, output, error) == 0,
            "四类收敛条件全部成立后应成功");
    require(factory.state->barrier_index == 2, "应等待 barrier ack");
    require(factory.state->convergence_index == 5,
            "Hash、Stream、published revision 和 MySQL 都应参与轮询");
    require(factory.state->mysql_deadlines.size() == 5 &&
                std::all_of(
                    factory.state->mysql_deadlines.begin(),
                    factory.state->mysql_deadlines.end(),
                    [&](const auto value) {
                        return value == *factory.opened_at + factory.timeout;
                    }),
            "MySQL count 必须复用管理命令的同一绝对 deadline");
    require(factory.state->applied.size() == 1 &&
                factory.state->applied.front().operation() ==
                    BlacklistMutationOperation::ClearAll,
            "ack 后只执行一次 CLEAR_ALL");
    require(output.str() ==
                "status=ok operation=CLEAR_ALL revision=4 barrier_token=1 converged=1\n",
            "clear 成功摘要");
    require(error.str().empty(), "成功不应报错");
}

void convergencePredicateRequiresAllStores() {
    MysqlBlacklistCountResult mysql = mysqlCountResult();
    auto redis = convergence(0, 0, 7, 7);
    require(aegisflow::tools::resetConverged(redis, mysql),
            "四层都为空且 revision 一致时才收敛");

    redis.user_count = 1;
    require(!aegisflow::tools::resetConverged(redis, mysql), "user Hash");
    redis.user_count = 0;
    redis.ip_count = 1;
    require(!aegisflow::tools::resetConverged(redis, mysql), "IP Hash");
    redis.ip_count = 0;
    redis.device_count = 1;
    require(!aegisflow::tools::resetConverged(redis, mysql), "device Hash");
    redis.device_count = 0;
    redis.pending_count = 1;
    require(!aegisflow::tools::resetConverged(redis, mysql), "pending Stream");
    redis.pending_count = 0;
    redis.published_revision = 6;
    require(!aegisflow::tools::resetConverged(redis, mysql), "published revision");
    redis.published_revision = 7;
    mysql.counts.ips = 1;
    require(!aegisflow::tools::resetConverged(redis, mysql), "MySQL active rows");
    mysql.counts.ips = 0;
    mysql.success = false;
    require(!aegisflow::tools::resetConverged(redis, mysql), "MySQL count error");
}

void runtimeFailuresReturnOne() {
    {
        ScriptedFactory factory;
        factory.open_error = "Redis 连接失败";
        FakeClock clock;
        std::ostringstream output;
        std::ostringstream error;
        const std::array arguments{
            std::string_view("disable"), std::string_view("--type"),
            std::string_view("user"), std::string_view("--id"),
            std::string_view("user-1"),
        };
        require(run(arguments, factory, clock, output, error) == 1,
                "连接失败应返回 1");
    }
    {
        ScriptedFactory factory;
        factory.state->apply_results = {
            storeResult(StoreStatus::CommitUnknown),
        };
        FakeClock clock;
        std::ostringstream output;
        std::ostringstream error;
        const std::array arguments{
            std::string_view("disable"), std::string_view("--type"),
            std::string_view("user"), std::string_view("--id"),
            std::string_view("user-1"),
        };
        require(run(arguments, factory, clock, output, error) == 1,
                "EXEC 后连接丢失不能当成成功");
        require(output.str().empty(), "运行失败无成功摘要");
    }
}

}  // namespace

int main() {
    return aegisflow::test::runModule("manage_blacklist", {
        {"validated command parsing", parserBuildsValidatedMutations},
        {"invalid option rejection", parserRejectsUnknownDuplicateAndMixedOptions},
        {"confirmation before connection", invalidClearNeverOpensBackend},
        {"dependency open shares deadline", dependencyOpenConsumesTheCommandDeadline},
        {"WATCH conflict retry", mutationRetriesWatchConflict},
        {"reset barrier cannot be bypassed", clearCannotBypassBarrier},
        {"four-layer convergence", clearWaitsForEveryConvergenceLayer},
        {"convergence predicate", convergencePredicateRequiresAllStores},
        {"runtime failure exit code", runtimeFailuresReturnOne},
    });
}
