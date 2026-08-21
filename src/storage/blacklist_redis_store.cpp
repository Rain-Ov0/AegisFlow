#include "aegisflow/storage/blacklist_redis_store.hpp"

#include "aegisflow/domain/ip_address.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace aegisflow::storage {
namespace {

constexpr std::uint64_t kMaxRedisInteger =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

StoreStatus commandStatus(const RedisCommandResult& result) noexcept {
    switch (result.status) {
    case RedisCommandStatus::Ok:
        return StoreStatus::Ok;
    case RedisCommandStatus::DeadlineExceeded:
        return StoreStatus::DeadlineExceeded;
    case RedisCommandStatus::IoError:
        return StoreStatus::IoError;
    case RedisCommandStatus::ProtocolError:
        return StoreStatus::ProtocolError;
    }
    return StoreStatus::IoError;
}

RedisCommandResult run(
    IRedisCommandExecutor& executor,
    std::initializer_list<std::string> argv,
    const RedisDeadline deadline
) {
    const std::vector<std::string> owned(argv);
    return executor.command(owned, deadline);
}

RedisCommandResult run(
    IRedisCommandExecutor& executor,
    const std::vector<std::string>& argv,
    const RedisDeadline deadline
) {
    return executor.command(argv, deadline);
}

bool statusIs(
    const RedisCommandResult& result,
    const std::string_view expected
) noexcept {
    return result.status == RedisCommandStatus::Ok &&
           result.value.kind == RedisValueKind::Status &&
           result.value.text == expected;
}

std::optional<std::uint64_t> parseUnsigned(
    const std::string_view text,
    const std::uint64_t maximum = kMaxRedisInteger
) noexcept {
    if (text.empty() || (text.size() > 1 && text.front() == '0')) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() ||
        value > maximum) {
        return std::nullopt;
    }
    return value;
}

bool isStreamId(const std::string_view value) noexcept {
    const auto separator = value.find('-');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 == value.size() ||
        value.find('-', separator + 1) != std::string_view::npos) {
        return false;
    }
    return parseUnsigned(value.substr(0, separator)).has_value() &&
           parseUnsigned(value.substr(separator + 1)).has_value();
}

std::string entityTypeText(const risk::EntityType type) {
    switch (type) {
    case risk::EntityType::User:
        return "USER";
    case risk::EntityType::Ip:
        return "IP";
    case risk::EntityType::Device:
        return "DEVICE";
    }
    return "";
}

std::optional<risk::EntityType> parseEntityType(
    const std::string_view text
) noexcept {
    if (text == "USER") {
        return risk::EntityType::User;
    }
    if (text == "IP") {
        return risk::EntityType::Ip;
    }
    if (text == "DEVICE") {
        return risk::EntityType::Device;
    }
    return std::nullopt;
}

const std::string& hashKey(
    const RedisKeySet& keys,
    const risk::EntityType type
) {
    switch (type) {
    case risk::EntityType::User:
        return keys.users;
    case risk::EntityType::Ip:
        return keys.ips;
    case risk::EntityType::Device:
        return keys.devices;
    }
    return keys.users;
}

StoreStatus ensureType(
    IRedisCommandExecutor& executor,
    const std::string& key,
    const std::string_view expected,
    const bool allow_none,
    const RedisDeadline deadline
) {
    const auto result = run(executor, {"TYPE", key}, deadline);
    const auto status = commandStatus(result);
    if (status != StoreStatus::Ok) {
        return status;
    }
    if (result.value.kind != RedisValueKind::Status) {
        return StoreStatus::ProtocolError;
    }
    if (result.value.text == expected ||
        (allow_none && result.value.text == "none")) {
        return StoreStatus::Ok;
    }
    return StoreStatus::InvalidRemoteState;
}

StoreStatus unwatch(
    IRedisCommandExecutor& executor,
    const RedisDeadline deadline
) {
    const auto result = run(executor, {"UNWATCH"}, deadline);
    const auto status = commandStatus(result);
    if (status != StoreStatus::Ok) {
        return status;
    }
    return statusIs(result, "OK") ? StoreStatus::Ok
                                   : StoreStatus::ProtocolError;
}

StoreStatus discard(
    IRedisCommandExecutor& executor,
    const RedisDeadline deadline
) {
    const auto result = run(executor, {"DISCARD"}, deadline);
    const auto status = commandStatus(result);
    if (status != StoreStatus::Ok) {
        return status;
    }
    return statusIs(result, "OK") ? StoreStatus::Ok
                                   : StoreStatus::ProtocolError;
}

class RedisStateGuard final {
public:
    explicit RedisStateGuard(IRedisCommandExecutor& executor) noexcept
        : executor_(&executor) {}
    ~RedisStateGuard() {
        if (armed_) {
            executor_->invalidate();
        }
    }

    RedisStateGuard(const RedisStateGuard&) = delete;
    RedisStateGuard& operator=(const RedisStateGuard&) = delete;

    void dismiss() noexcept { armed_ = false; }

private:
    IRedisCommandExecutor* executor_;
    bool armed_ = true;
};

StoreStatus leaveWatch(
    IRedisCommandExecutor& executor,
    RedisStateGuard& guard,
    const RedisDeadline deadline,
    const StoreStatus original
) {
    const auto cleaned = unwatch(executor, deadline);
    if (cleaned != StoreStatus::Ok) {
        executor.invalidate();
        guard.dismiss();
        return cleaned;
    }
    guard.dismiss();
    return original;
}

StoreStatus leaveTransaction(
    IRedisCommandExecutor& executor,
    RedisStateGuard& guard,
    const RedisDeadline deadline,
    const StoreStatus original
) {
    const auto cleaned = discard(executor, deadline);
    if (cleaned != StoreStatus::Ok) {
        executor.invalidate();
        guard.dismiss();
        return cleaned;
    }
    guard.dismiss();
    return original;
}

struct ReadyRevision {
    StoreStatus status = StoreStatus::IoError;
    std::uint64_t revision = 0;
};

StoreResult storeResult(
    const StoreStatus status,
    std::optional<std::uint64_t> revision = std::nullopt
) {
    StoreResult result;
    result.status = status;
    result.revision = revision;
    return result;
}

ReadyRevision readyRevision(
    const StoreStatus status,
    const std::uint64_t revision = 0
) {
    ReadyRevision result;
    result.status = status;
    result.revision = revision;
    return result;
}

PendingReadResult pendingReadResult(const StoreStatus status) {
    PendingReadResult result;
    result.status = status;
    return result;
}

SnapshotReadResult snapshotReadResult(const StoreStatus status) {
    SnapshotReadResult result;
    result.status = status;
    return result;
}

ResetReadResult resetReadResult(const StoreStatus status) {
    ResetReadResult result;
    result.status = status;
    return result;
}

ReadyRevision readWatchedReadyRevision(
    IRedisCommandExecutor& executor,
    const RedisKeySet& keys,
    const base::ArrayView<const std::string> hash_keys,
    const RedisDeadline deadline
) {
    auto status = ensureType(
        executor, keys.cache_ready, "string", true, deadline);
    if (status != StoreStatus::Ok) {
        return readyRevision(status);
    }
    const auto ready = run(
        executor, {"GET", keys.cache_ready}, deadline);
    status = commandStatus(ready);
    if (status != StoreStatus::Ok) {
        return readyRevision(status);
    }
    if (ready.value.kind == RedisValueKind::Nil) {
        return readyRevision(StoreStatus::NotReady);
    }
    if (ready.value.kind != RedisValueKind::String ||
        ready.value.text != "1") {
        return readyRevision(StoreStatus::InvalidRemoteState);
    }

    for (const auto& [key, expected, allow_none] : std::array{
             std::tuple{std::cref(keys.pending), "stream", true},
             std::tuple{std::cref(keys.revision), "string", false},
         }) {
        status = ensureType(
            executor, key.get(), expected, allow_none, deadline);
        if (status != StoreStatus::Ok) {
            return readyRevision(status);
        }
    }
    for (const auto& key : hash_keys) {
        status = ensureType(executor, key, "hash", true, deadline);
        if (status != StoreStatus::Ok) {
            return readyRevision(status);
        }
    }

    const auto revision = run(
        executor, {"GET", keys.revision}, deadline);
    status = commandStatus(revision);
    if (status != StoreStatus::Ok) {
        return readyRevision(status);
    }
    if (revision.value.kind != RedisValueKind::String) {
        return readyRevision(StoreStatus::InvalidRemoteState);
    }
    const auto parsed = parseUnsigned(
        revision.value.text, kMaxRedisInteger - 1);
    if (!parsed.has_value()) {
        return readyRevision(StoreStatus::InvalidRemoteState);
    }
    return readyRevision(StoreStatus::Ok, *parsed);
}

StoreStatus queueCommand(
    IRedisCommandExecutor& executor,
    const std::vector<std::string>& argv,
    const RedisDeadline deadline
) {
    const auto result = run(executor, argv, deadline);
    const auto status = commandStatus(result);
    if (status != StoreStatus::Ok) {
        return status;
    }
    return statusIs(result, "QUEUED") ? StoreStatus::Ok
                                       : StoreStatus::ProtocolError;
}

StoreStatus execBaseStatus(const RedisCommandResult& result) noexcept {
    if (result.status == RedisCommandStatus::DeadlineExceeded) {
        return StoreStatus::DeadlineExceeded;
    }
    if (result.status == RedisCommandStatus::IoError) {
        // EXEC 已交给 hiredis，空 reply 无法区分服务端未执行与已提交但
        // 响应丢失，上层不得当成可安全重试的“未写入”。
        return StoreStatus::CommitUnknown;
    }
    if (result.status == RedisCommandStatus::ProtocolError) {
        return StoreStatus::ProtocolError;
    }
    if (result.value.kind == RedisValueKind::Nil) {
        return StoreStatus::WatchConflict;
    }
    if (result.value.kind == RedisValueKind::Error) {
        return StoreStatus::TransactionError;
    }
    if (result.value.kind != RedisValueKind::Array) {
        return StoreStatus::ProtocolError;
    }
    for (const auto& value : result.value.elements) {
        if (value.kind == RedisValueKind::Error) {
            return StoreStatus::TransactionError;
        }
    }
    return StoreStatus::Ok;
}

StoreStatus checkedExecStatus(
    IRedisCommandExecutor& executor,
    RedisStateGuard& guard,
    const RedisCommandResult& result
) noexcept {
    const auto status = execBaseStatus(result);
    if (result.status == RedisCommandStatus::Ok) {
        // EXEC 有 reply 即表示 Redis 已退出 MULTI；回复内容仍会在
        // 上层严格校验，但连接不再携带事务状态。
        guard.dismiss();
    } else {
        executor.invalidate();
        guard.dismiss();
    }
    return status;
}

std::vector<std::string> streamFields(
    const risk::BlacklistMutation& mutation
) {
    switch (mutation.operation()) {
    case risk::BlacklistMutationOperation::Upsert:
        return {
            "operation", "UPSERT",
            "type", entityTypeText(*mutation.entityType()),
            "id", mutation.id(),
            "reason", mutation.reason(),
            "expire_at_ms", std::to_string(mutation.expireAtMs()),
        };
    case risk::BlacklistMutationOperation::Disable:
        return {
            "operation", "DISABLE",
            "type", entityTypeText(*mutation.entityType()),
            "id", mutation.id(),
        };
    case risk::BlacklistMutationOperation::ClearAll:
        return {"operation", "CLEAR_ALL"};
    }
    return {};
}

std::optional<risk::BlacklistMutation> parseStreamMutation(
    const RedisValue& fields,
    std::string& error
) {
    if (fields.kind != RedisValueKind::Array ||
        fields.elements.size() % 2 != 0) {
        error = "field array must contain pairs";
        return std::nullopt;
    }
    std::map<std::string, std::string, std::less<>> values;
    for (std::size_t index = 0; index < fields.elements.size(); index += 2) {
        const auto& key = fields.elements[index];
        const auto& value = fields.elements[index + 1];
        if (key.kind != RedisValueKind::String ||
            value.kind != RedisValueKind::String ||
            !values.emplace(key.text, value.text).second) {
            error = "field name/value invalid or duplicated";
            return std::nullopt;
        }
    }

    const auto operation = values.find("operation");
    if (operation == values.end()) {
        error = "operation missing";
        return std::nullopt;
    }
    if (operation->second == "CLEAR_ALL") {
        if (values.size() != 1) {
            error = "CLEAR_ALL contains extra fields";
            return std::nullopt;
        }
        return risk::BlacklistMutation::clearAll();
    }

    const auto type_value = values.find("type");
    const auto id = values.find("id");
    if (type_value == values.end() || id == values.end()) {
        error = "type or id missing";
        return std::nullopt;
    }
    const auto type = parseEntityType(type_value->second);
    if (!type.has_value()) {
        error = "entity type invalid";
        return std::nullopt;
    }
    if (operation->second == "DISABLE") {
        if (values.size() != 3) {
            error = "DISABLE contains unknown or extra fields";
            return std::nullopt;
        }
        auto mutation = risk::BlacklistMutation::disable(*type, id->second);
        if (!mutation.has_value()) {
            error = "DISABLE entity invalid";
        }
        return mutation;
    }
    if (operation->second != "UPSERT" || values.size() != 5) {
        error = "operation invalid or UPSERT fields incomplete";
        return std::nullopt;
    }
    const auto reason = values.find("reason");
    const auto expiry = values.find("expire_at_ms");
    if (reason == values.end() || expiry == values.end()) {
        error = "UPSERT reason or expiry missing";
        return std::nullopt;
    }
    const auto expire_at_ms = parseUnsigned(
        expiry->second, risk::BlacklistMutation::kMaxExpireAtMs);
    if (!expire_at_ms.has_value()) {
        error = "UPSERT expiry invalid";
        return std::nullopt;
    }
    auto mutation = risk::BlacklistMutation::upsert(
        *type, id->second, reason->second, *expire_at_ms);
    if (!mutation.has_value()) {
        error = "UPSERT entity or reason invalid";
    }
    return mutation;
}

StoreStatus parseHashScan(
    const RedisCommandResult& result,
    const risk::EntityType type,
    std::string& cursor,
    std::vector<risk::BlacklistEntry>& entries
) {
    const auto status = commandStatus(result);
    if (status != StoreStatus::Ok) {
        return status;
    }
    if (result.value.kind == RedisValueKind::Error) {
        return StoreStatus::InvalidRemoteState;
    }
    if (result.value.kind != RedisValueKind::Array ||
        result.value.elements.size() != 2 ||
        result.value.elements[0].kind != RedisValueKind::String ||
        result.value.elements[1].kind != RedisValueKind::Array ||
        result.value.elements[1].elements.size() % 2 != 0) {
        return StoreStatus::ProtocolError;
    }
    cursor = result.value.elements[0].text;
    if (!parseUnsigned(cursor).has_value()) {
        return StoreStatus::ProtocolError;
    }
    const auto& values = result.value.elements[1].elements;
    for (std::size_t index = 0; index < values.size(); index += 2) {
        if (values[index].kind != RedisValueKind::String ||
            values[index + 1].kind != RedisValueKind::String) {
            return StoreStatus::ProtocolError;
        }
        std::string id = values[index].text;
        if (type == risk::EntityType::Ip) {
            const auto address = domain::IpAddress::parse(id);
            if (!address.has_value() || address->canonicalText() != id) {
                return StoreStatus::InvalidRemoteState;
            }
        } else if (id.empty() ||
                   id.size() > risk::BlacklistMutation::kMaxEntityIdBytes ||
                   id.find('\0') != std::string::npos) {
            return StoreStatus::InvalidRemoteState;
        }
        const auto expiry = parseUnsigned(
            values[index + 1].text,
            risk::BlacklistMutation::kMaxExpireAtMs);
        if (!expiry.has_value()) {
            return StoreStatus::InvalidRemoteState;
        }
        entries.push_back({type, std::move(id), *expiry});
    }
    return StoreStatus::Ok;
}

}  // 命名空间

RedisKeySet RedisKeySet::fromPrefix(std::string prefix) {
    RedisKeySet keys;
    keys.users = prefix + ":user";
    keys.ips = prefix + ":ip";
    keys.devices = prefix + ":device";
    keys.pending = prefix + ":pending";
    keys.revision = prefix + ":revision";
    keys.cache_ready = prefix + ":cache_ready";
    keys.published_revision = prefix + ":published_revision";
    keys.reset_barrier = prefix + ":reset_barrier";
    return keys;
}

std::vector<std::string> RedisKeySet::all() const {
    return {users, ips, devices, pending, revision, cache_ready,
            published_revision, reset_barrier};
}

bool ResetConvergence::redisAndPublicationEmpty() const noexcept {
    return status == StoreStatus::Ok && user_count == 0 && ip_count == 0 &&
           device_count == 0 && pending_count == 0 && revision.has_value() &&
           published_revision == revision;
}

BlacklistRedisStore::BlacklistRedisStore(
    IRedisCommandExecutor& executor,
    RedisKeySet keys
)
    : executor_(&executor), keys_(std::move(keys)) {}

StoreResult BlacklistRedisStore::applyMutations(
    const base::ArrayView<const risk::BlacklistMutation> mutations,
    const RedisDeadline deadline
) {
    if (mutations.empty()) {
        return storeResult(StoreStatus::ProtocolError);
    }
    const bool clear =
        mutations[0].operation() ==
        risk::BlacklistMutationOperation::ClearAll;
    if ((clear && mutations.size() != 1) ||
        (!clear && std::any_of(
             mutations.begin(), mutations.end(), [](const auto& mutation) {
                 return mutation.operation() ==
                        risk::BlacklistMutationOperation::ClearAll;
             }))) {
        return storeResult(StoreStatus::ProtocolError);
    }

    std::set<std::string> touched;
    if (clear) {
        touched.insert(keys_.users);
        touched.insert(keys_.ips);
        touched.insert(keys_.devices);
    } else {
        for (const auto& mutation : mutations) {
            if (!mutation.entityType().has_value()) {
                return storeResult(StoreStatus::ProtocolError);
            }
            touched.insert(hashKey(keys_, *mutation.entityType()));
        }
    }

    std::vector<std::string> watch{
        "WATCH", keys_.cache_ready, keys_.pending, keys_.revision};
    watch.insert(watch.end(), touched.begin(), touched.end());
    const auto watched = run(*executor_, watch, deadline);
    auto status = commandStatus(watched);
    if (status != StoreStatus::Ok) {
        return storeResult(status);
    }
    if (!statusIs(watched, "OK")) {
        executor_->invalidate();
        return storeResult(StoreStatus::ProtocolError);
    }
    RedisStateGuard state_guard(*executor_);

    const std::vector<std::string> touched_vector(
        touched.begin(), touched.end());
    const auto precondition = readWatchedReadyRevision(
        *executor_, keys_, touched_vector, deadline);
    if (precondition.status != StoreStatus::Ok) {
        return storeResult(leaveWatch(
            *executor_, state_guard, deadline, precondition.status));
    }

    const auto multi = run(*executor_, {"MULTI"}, deadline);
    status = commandStatus(multi);
    if (status != StoreStatus::Ok || !statusIs(multi, "OK")) {
        const auto original = status == StoreStatus::Ok
            ? StoreStatus::ProtocolError
            : status;
        return storeResult(leaveTransaction(
            *executor_, state_guard, deadline, original));
    }

    std::size_t expected_results = 0;
    const auto queueOrDiscard = [&](const std::vector<std::string>& argv) {
        const auto queued = queueCommand(*executor_, argv, deadline);
        if (queued != StoreStatus::Ok) {
            return leaveTransaction(
                *executor_, state_guard, deadline, queued);
        }
        ++expected_results;
        return StoreStatus::Ok;
    };

    if (clear) {
        status = queueOrDiscard({
            "DEL", keys_.users, keys_.ips, keys_.devices});
        if (status == StoreStatus::Ok) {
            auto fields = streamFields(mutations[0]);
            std::vector<std::string> xadd{"XADD", keys_.pending, "*"};
            xadd.insert(xadd.end(), fields.begin(), fields.end());
            status = queueOrDiscard(xadd);
        }
    } else {
        for (const auto& mutation : mutations) {
            const auto& key = hashKey(keys_, *mutation.entityType());
            if (mutation.operation() ==
                risk::BlacklistMutationOperation::Upsert) {
                status = queueOrDiscard({
                    "HSET", key, mutation.id(),
                    std::to_string(mutation.expireAtMs())});
            } else {
                status = queueOrDiscard({"HDEL", key, mutation.id()});
            }
            if (status != StoreStatus::Ok) {
                return storeResult(status);
            }
            auto fields = streamFields(mutation);
            std::vector<std::string> xadd{"XADD", keys_.pending, "*"};
            xadd.insert(xadd.end(), fields.begin(), fields.end());
            status = queueOrDiscard(xadd);
            if (status != StoreStatus::Ok) {
                return storeResult(status);
            }
        }
    }
    if (status != StoreStatus::Ok) {
        return storeResult(status);
    }
    status = queueOrDiscard({"INCR", keys_.revision});
    if (status != StoreStatus::Ok) {
        return storeResult(status);
    }

    const auto executed = run(*executor_, {"EXEC"}, deadline);
    status = checkedExecStatus(*executor_, state_guard, executed);
    if (status != StoreStatus::Ok) {
        return storeResult(status);
    }
    if (executed.value.elements.size() != expected_results) {
        return storeResult(StoreStatus::ProtocolError);
    }

    for (std::size_t index = 0; index + 1 < expected_results; ++index) {
        const auto& value = executed.value.elements[index];
        const bool is_xadd = clear ? index == 1 : index % 2 == 1;
        if (is_xadd) {
            if (value.kind != RedisValueKind::String || value.text.empty()) {
                return storeResult(StoreStatus::ProtocolError);
            }
        } else if (value.kind != RedisValueKind::Integer ||
                   value.integer < 0 ||
                   (clear && value.integer > 3)) {
            return storeResult(StoreStatus::ProtocolError);
        }
    }
    const auto& revision = executed.value.elements.back();
    const auto expected_revision = precondition.revision + 1;
    if (revision.kind != RedisValueKind::Integer || revision.integer < 0 ||
        static_cast<std::uint64_t>(revision.integer) != expected_revision) {
        return storeResult(StoreStatus::ProtocolError);
    }
    return storeResult(StoreStatus::Ok, expected_revision);
}

PendingReadResult BlacklistRedisStore::readPending(
    const std::size_t count,
    const RedisDeadline deadline
) {
    if (count == 0) {
        return pendingReadResult(StoreStatus::ProtocolError);
    }
    const auto result = run(
        *executor_,
        {"XRANGE", keys_.pending, "-", "+", "COUNT", std::to_string(count)},
        deadline);
    auto status = commandStatus(result);
    if (status != StoreStatus::Ok) {
        return pendingReadResult(status);
    }
    if (result.value.kind == RedisValueKind::Error) {
        return pendingReadResult(StoreStatus::InvalidRemoteState);
    }
    if (result.value.kind != RedisValueKind::Array) {
        return pendingReadResult(StoreStatus::ProtocolError);
    }

    PendingReadResult output = pendingReadResult(StoreStatus::Ok);
    output.entries.reserve(result.value.elements.size());
    for (const auto& raw_entry : result.value.elements) {
        if (raw_entry.kind != RedisValueKind::Array ||
            raw_entry.elements.size() != 2 ||
            raw_entry.elements[0].kind != RedisValueKind::String) {
            return pendingReadResult(StoreStatus::ProtocolError);
        }
        PendingBlacklistEntry entry;
        entry.stream_id = raw_entry.elements[0].text;
        if (!isStreamId(entry.stream_id)) {
            return pendingReadResult(StoreStatus::ProtocolError);
        }
        entry.mutation = parseStreamMutation(raw_entry.elements[1], entry.error);
        output.entries.push_back(std::move(entry));
    }
    return output;
}

StoreStatus BlacklistRedisStore::deletePending(
    const base::ArrayView<const std::string> stream_ids,
    const RedisDeadline deadline
) {
    if (stream_ids.empty()) {
        return StoreStatus::Ok;
    }
    if (std::any_of(stream_ids.begin(), stream_ids.end(), [](const auto& id) {
            return !isStreamId(id);
        })) {
        return StoreStatus::ProtocolError;
    }
    std::vector<std::string> command{"XDEL", keys_.pending};
    command.insert(command.end(), stream_ids.begin(), stream_ids.end());
    const auto result = run(*executor_, command, deadline);
    const auto status = commandStatus(result);
    if (status != StoreStatus::Ok) {
        return status;
    }
    if (result.value.kind == RedisValueKind::Error) {
        return StoreStatus::InvalidRemoteState;
    }
    return result.value.kind == RedisValueKind::Integer &&
                   result.value.integer >= 0
               ? StoreStatus::Ok
               : StoreStatus::ProtocolError;
}

StoreResult BlacklistRedisStore::readReadyRevision(
    const RedisDeadline deadline
) {
    const std::array<std::string, 3> hashes{
        keys_.users, keys_.ips, keys_.devices};
    const auto result = readWatchedReadyRevision(
        *executor_, keys_, hashes, deadline);
    return storeResult(
        result.status,
        result.status == StoreStatus::Ok
            ? std::optional<std::uint64_t>{result.revision}
            : std::nullopt);
}

StoreResult BlacklistRedisStore::readRevision(const RedisDeadline deadline) {
    const auto result = run(*executor_, {"GET", keys_.revision}, deadline);
    const auto status = commandStatus(result);
    if (status != StoreStatus::Ok) {
        return storeResult(status);
    }
    if (result.value.kind != RedisValueKind::String) {
        return storeResult(StoreStatus::InvalidRemoteState);
    }
    const auto revision = parseUnsigned(result.value.text);
    return revision.has_value()
        ? storeResult(StoreStatus::Ok, *revision)
        : storeResult(StoreStatus::InvalidRemoteState);
}

SnapshotReadResult BlacklistRedisStore::loadStableSnapshot(
    const std::size_t scan_count,
    const RedisDeadline deadline
) {
    if (scan_count == 0) {
        return snapshotReadResult(StoreStatus::ProtocolError);
    }
    const auto before = readRevision(deadline);
    if (before.status != StoreStatus::Ok || !before.revision.has_value()) {
        return snapshotReadResult(before.status);
    }

    SnapshotReadResult output = snapshotReadResult(StoreStatus::Ok);
    output.revision = *before.revision;
    for (const auto& [type, key] : std::array{
             std::pair{risk::EntityType::User, std::cref(keys_.users)},
             std::pair{risk::EntityType::Ip, std::cref(keys_.ips)},
             std::pair{risk::EntityType::Device, std::cref(keys_.devices)},
         }) {
        std::string cursor = "0";
        do {
            if (std::chrono::steady_clock::now() >= deadline) {
                return snapshotReadResult(StoreStatus::DeadlineExceeded);
            }
            const auto result = run(
                *executor_,
                {"HSCAN", key.get(), cursor, "COUNT",
                 std::to_string(scan_count)},
                deadline);
            const auto status = parseHashScan(
                result, type, cursor, output.entries);
            if (status != StoreStatus::Ok) {
                return snapshotReadResult(status);
            }
        } while (cursor != "0");
    }

    const auto after = readRevision(deadline);
    if (after.status != StoreStatus::Ok || !after.revision.has_value()) {
        return snapshotReadResult(after.status);
    }
    if (*after.revision != *before.revision) {
        return snapshotReadResult(StoreStatus::WatchConflict);
    }
    return output;
}

StoreResult BlacklistRedisStore::removeExpiredFields(
    const base::ArrayView<const risk::BlacklistEntry> expired_entries,
    const std::uint64_t expected_revision,
    const std::uint64_t now_ms,
    const RedisDeadline deadline
) {
    if (expired_entries.empty() || expected_revision >= kMaxRedisInteger) {
        return storeResult(StoreStatus::ProtocolError);
    }

    using ExpiryById = std::map<std::string, std::uint64_t, std::less<>>;
    std::array<ExpiryById, 3> grouped;
    const auto typeIndex = [](const risk::EntityType type)
        -> std::optional<std::size_t> {
        switch (type) {
        case risk::EntityType::User:
            return 0;
        case risk::EntityType::Ip:
            return 1;
        case risk::EntityType::Device:
            return 2;
        }
        return std::nullopt;
    };

    for (const auto& entry : expired_entries) {
        const auto index = typeIndex(entry.type);
        const auto normalized = risk::BlacklistMutation::disable(
            entry.type, entry.id);
        if (!index.has_value() || !normalized.has_value() ||
            normalized->id() != entry.id || entry.expire_at_ms == 0 ||
            entry.expire_at_ms > now_ms ||
            entry.expire_at_ms > risk::BlacklistMutation::kMaxExpireAtMs ||
            !grouped[*index]
                 .emplace(entry.id, entry.expire_at_ms)
                 .second) {
            return storeResult(StoreStatus::ProtocolError);
        }
    }

    const std::array<std::reference_wrapper<const std::string>, 3> hash_keys{
        keys_.users, keys_.ips, keys_.devices};
    std::vector<std::string> watch{"WATCH", keys_.revision};
    for (std::size_t group = 0; group < grouped.size(); ++group) {
        if (!grouped[group].empty()) {
            watch.push_back(hash_keys[group].get());
        }
    }

    auto result = run(*executor_, watch, deadline);
    auto status = commandStatus(result);
    if (status != StoreStatus::Ok || !statusIs(result, "OK")) {
        if (status == StoreStatus::Ok) {
            executor_->invalidate();
        }
        return storeResult(
            status == StoreStatus::Ok ? StoreStatus::ProtocolError : status);
    }
    RedisStateGuard state_guard(*executor_);

    status = ensureType(
        *executor_, keys_.revision, "string", false, deadline);
    if (status != StoreStatus::Ok) {
        return storeResult(leaveWatch(
            *executor_, state_guard, deadline, status));
    }
    for (std::size_t group = 0; group < grouped.size(); ++group) {
        if (grouped[group].empty()) {
            continue;
        }
        status = ensureType(
            *executor_, hash_keys[group].get(), "hash", true, deadline);
        if (status != StoreStatus::Ok) {
            return storeResult(leaveWatch(
                *executor_, state_guard, deadline, status));
        }
    }

    result = run(*executor_, {"GET", keys_.revision}, deadline);
    status = commandStatus(result);
    if (status != StoreStatus::Ok) {
        return storeResult(leaveWatch(
            *executor_, state_guard, deadline, status));
    }
    if (result.value.kind != RedisValueKind::String) {
        return storeResult(leaveWatch(
            *executor_, state_guard, deadline,
            StoreStatus::InvalidRemoteState));
    }
    const auto current_revision = parseUnsigned(
        result.value.text, kMaxRedisInteger - 1);
    if (!current_revision.has_value()) {
        return storeResult(leaveWatch(
            *executor_, state_guard, deadline,
            StoreStatus::InvalidRemoteState));
    }
    if (*current_revision != expected_revision) {
        return storeResult(leaveWatch(
            *executor_, state_guard, deadline,
            StoreStatus::WatchConflict));
    }

    // HSCAN 之后条目可能被续期或删除。WATCH 保护整个 Hash，
    // HGET 又逐项核对扫描时的过期值；任一值变化就整批放弃，
    // 不会用旧快照删掉已续期的黑名单。
    for (std::size_t group = 0; group < grouped.size(); ++group) {
        for (const auto& [id, scanned_expiry] : grouped[group]) {
            result = run(
                *executor_, {"HGET", hash_keys[group].get(), id}, deadline);
            status = commandStatus(result);
            if (status != StoreStatus::Ok) {
                return storeResult(leaveWatch(
                    *executor_, state_guard, deadline, status));
            }
            if (result.value.kind == RedisValueKind::Nil) {
                return storeResult(leaveWatch(
                    *executor_, state_guard, deadline,
                    StoreStatus::WatchConflict));
            }
            if (result.value.kind != RedisValueKind::String) {
                return storeResult(leaveWatch(
                    *executor_, state_guard, deadline,
                    StoreStatus::InvalidRemoteState));
            }
            const auto remote_expiry = parseUnsigned(
                result.value.text,
                risk::BlacklistMutation::kMaxExpireAtMs);
            if (!remote_expiry.has_value()) {
                return storeResult(leaveWatch(
                    *executor_, state_guard, deadline,
                    StoreStatus::InvalidRemoteState));
            }
            if (*remote_expiry != scanned_expiry ||
                *remote_expiry == 0 || *remote_expiry > now_ms) {
                return storeResult(leaveWatch(
                    *executor_, state_guard, deadline,
                    StoreStatus::WatchConflict));
            }
        }
    }

    result = run(*executor_, {"MULTI"}, deadline);
    status = commandStatus(result);
    if (status != StoreStatus::Ok || !statusIs(result, "OK")) {
        const auto original = status == StoreStatus::Ok
            ? StoreStatus::ProtocolError
            : status;
        return storeResult(leaveTransaction(
            *executor_, state_guard, deadline, original));
    }

    std::vector<std::size_t> deleted_per_reply;
    for (std::size_t group = 0; group < grouped.size(); ++group) {
        if (grouped[group].empty()) {
            continue;
        }
        std::vector<std::string> hdel{"HDEL", hash_keys[group].get()};
        for (const auto& [id, expiry] : grouped[group]) {
            static_cast<void>(expiry);
            hdel.push_back(id);
        }
        status = queueCommand(*executor_, hdel, deadline);
        if (status != StoreStatus::Ok) {
            return storeResult(leaveTransaction(
                *executor_, state_guard, deadline, status));
        }
        deleted_per_reply.push_back(grouped[group].size());
    }
    status = queueCommand(
        *executor_, {"INCR", keys_.revision}, deadline);
    if (status != StoreStatus::Ok) {
        return storeResult(leaveTransaction(
            *executor_, state_guard, deadline, status));
    }

    result = run(*executor_, {"EXEC"}, deadline);
    status = checkedExecStatus(*executor_, state_guard, result);
    if (status != StoreStatus::Ok) {
        return storeResult(status);
    }
    if (result.value.elements.size() != deleted_per_reply.size() + 1) {
        return storeResult(StoreStatus::ProtocolError);
    }
    for (std::size_t index = 0; index < deleted_per_reply.size(); ++index) {
        const auto& reply = result.value.elements[index];
        if (reply.kind != RedisValueKind::Integer || reply.integer < 0 ||
            static_cast<std::uint64_t>(reply.integer) !=
                deleted_per_reply[index]) {
            return storeResult(StoreStatus::ProtocolError);
        }
    }
    const auto& revision = result.value.elements.back();
    const auto next_revision = expected_revision + 1;
    if (revision.kind != RedisValueKind::Integer || revision.integer < 0 ||
        static_cast<std::uint64_t>(revision.integer) != next_revision) {
        return storeResult(StoreStatus::ProtocolError);
    }
    return storeResult(StoreStatus::Ok, next_revision);
}

StoreStatus BlacklistRedisStore::setPublishedRevision(
    const std::uint64_t revision,
    const RedisDeadline deadline
) {
    if (revision > kMaxRedisInteger) {
        return StoreStatus::ProtocolError;
    }
    auto result = run(
        *executor_, {"WATCH", keys_.published_revision}, deadline);
    auto status = commandStatus(result);
    if (status != StoreStatus::Ok || !statusIs(result, "OK")) {
        if (status == StoreStatus::Ok) {
            executor_->invalidate();
        }
        return status == StoreStatus::Ok ? StoreStatus::ProtocolError : status;
    }
    RedisStateGuard state_guard(*executor_);
    status = ensureType(
        *executor_, keys_.published_revision, "string", true, deadline);
    if (status != StoreStatus::Ok) {
        return leaveWatch(*executor_, state_guard, deadline, status);
    }
    result = run(*executor_, {"MULTI"}, deadline);
    status = commandStatus(result);
    if (status != StoreStatus::Ok || !statusIs(result, "OK")) {
        const auto original = status == StoreStatus::Ok
            ? StoreStatus::ProtocolError
            : status;
        return leaveTransaction(*executor_, state_guard, deadline, original);
    }
    status = queueCommand(
        *executor_,
        {"SET", keys_.published_revision, std::to_string(revision)},
        deadline);
    if (status != StoreStatus::Ok) {
        return leaveTransaction(*executor_, state_guard, deadline, status);
    }
    result = run(*executor_, {"EXEC"}, deadline);
    status = checkedExecStatus(*executor_, state_guard, result);
    if (status != StoreStatus::Ok) {
        return status;
    }
    return result.value.elements.size() == 1 &&
                   result.value.elements.front().kind == RedisValueKind::Status &&
                   result.value.elements.front().text == "OK"
               ? StoreStatus::Ok
               : StoreStatus::ProtocolError;
}

StoreResult BlacklistRedisStore::requestResetBarrier(
    const RedisDeadline deadline
) {
    const auto result = run(
        *executor_,
        {"HINCRBY", keys_.reset_barrier, "requested", "1"},
        deadline);
    const auto status = commandStatus(result);
    if (status != StoreStatus::Ok) {
        return storeResult(status);
    }
    if (result.value.kind == RedisValueKind::Error) {
        return storeResult(StoreStatus::InvalidRemoteState);
    }
    if (result.value.kind != RedisValueKind::Integer ||
        result.value.integer <= 0) {
        return storeResult(StoreStatus::ProtocolError);
    }
    return storeResult(
        StoreStatus::Ok,
        static_cast<std::uint64_t>(result.value.integer));
}

ResetReadResult BlacklistRedisStore::readResetBarrier(
    const RedisDeadline deadline
) {
    const auto result = run(
        *executor_,
        {"HMGET", keys_.reset_barrier, "requested", "completed"},
        deadline);
    const auto status = commandStatus(result);
    if (status != StoreStatus::Ok) {
        return resetReadResult(status);
    }
    if (result.value.kind == RedisValueKind::Error) {
        return resetReadResult(StoreStatus::InvalidRemoteState);
    }
    if (result.value.kind != RedisValueKind::Array ||
        result.value.elements.size() != 2) {
        return resetReadResult(StoreStatus::ProtocolError);
    }
    std::array<std::uint64_t, 2> values{};
    for (std::size_t index = 0; index < 2; ++index) {
        const auto& value = result.value.elements[index];
        if (value.kind == RedisValueKind::Nil) {
            values[index] = 0;
            continue;
        }
        if (value.kind != RedisValueKind::String) {
            return resetReadResult(StoreStatus::InvalidRemoteState);
        }
        const auto parsed = parseUnsigned(value.text);
        if (!parsed.has_value()) {
            return resetReadResult(StoreStatus::InvalidRemoteState);
        }
        values[index] = *parsed;
    }
    if (values[1] > values[0]) {
        return resetReadResult(StoreStatus::InvalidRemoteState);
    }
    ResetReadResult output = resetReadResult(StoreStatus::Ok);
    output.state.requested = values[0];
    output.state.completed = values[1];
    return output;
}

StoreStatus BlacklistRedisStore::completeResetBarrier(
    const std::uint64_t token,
    const RedisDeadline deadline
) {
    if (token == 0 || token > kMaxRedisInteger) {
        return StoreStatus::ProtocolError;
    }
    auto result = run(
        *executor_, {"WATCH", keys_.reset_barrier}, deadline);
    auto status = commandStatus(result);
    if (status != StoreStatus::Ok || !statusIs(result, "OK")) {
        if (status == StoreStatus::Ok) {
            executor_->invalidate();
        }
        return status == StoreStatus::Ok ? StoreStatus::ProtocolError : status;
    }
    RedisStateGuard state_guard(*executor_);
    const auto state = readResetBarrier(deadline);
    if (state.status != StoreStatus::Ok ||
        token > state.state.requested || token < state.state.completed) {
        const auto original = state.status == StoreStatus::Ok
            ? StoreStatus::InvalidRemoteState
            : state.status;
        return leaveWatch(*executor_, state_guard, deadline, original);
    }
    result = run(*executor_, {"MULTI"}, deadline);
    status = commandStatus(result);
    if (status != StoreStatus::Ok || !statusIs(result, "OK")) {
        const auto original = status == StoreStatus::Ok
            ? StoreStatus::ProtocolError
            : status;
        return leaveTransaction(*executor_, state_guard, deadline, original);
    }
    status = queueCommand(
        *executor_,
        {"HSET", keys_.reset_barrier, "completed", std::to_string(token)},
        deadline);
    if (status != StoreStatus::Ok) {
        return leaveTransaction(*executor_, state_guard, deadline, status);
    }
    result = run(*executor_, {"EXEC"}, deadline);
    status = checkedExecStatus(*executor_, state_guard, result);
    if (status != StoreStatus::Ok) {
        return status;
    }
    if (result.value.elements.size() != 1 ||
        result.value.elements.front().kind != RedisValueKind::Integer ||
        result.value.elements.front().integer < 0) {
        return StoreStatus::ProtocolError;
    }
    return StoreStatus::Ok;
}

ResetConvergence BlacklistRedisStore::readResetConvergence(
    const RedisDeadline deadline
) {
    ResetConvergence output;
    output.status = StoreStatus::Ok;
    for (auto [key, destination, command] : std::array{
             std::tuple{std::cref(keys_.users), &output.user_count, "HLEN"},
             std::tuple{std::cref(keys_.ips), &output.ip_count, "HLEN"},
             std::tuple{std::cref(keys_.devices), &output.device_count, "HLEN"},
             std::tuple{std::cref(keys_.pending), &output.pending_count, "XLEN"},
         }) {
        const auto result = run(
            *executor_, {command, key.get()}, deadline);
        const auto status = commandStatus(result);
        if (status != StoreStatus::Ok) {
            output.status = status;
            return output;
        }
        if (result.value.kind == RedisValueKind::Error) {
            output.status = StoreStatus::InvalidRemoteState;
            return output;
        }
        if (result.value.kind != RedisValueKind::Integer ||
            result.value.integer < 0) {
            output.status = StoreStatus::ProtocolError;
            return output;
        }
        *destination = static_cast<std::uint64_t>(result.value.integer);
    }

    const auto parseRevisionKey = [&](const std::string& key)
        -> std::pair<StoreStatus, std::optional<std::uint64_t>> {
        const auto result = run(*executor_, {"GET", key}, deadline);
        auto status = commandStatus(result);
        if (status != StoreStatus::Ok) {
            return {status, std::nullopt};
        }
        if (result.value.kind == RedisValueKind::Nil) {
            return {StoreStatus::Ok, std::nullopt};
        }
        if (result.value.kind != RedisValueKind::String) {
            return {StoreStatus::InvalidRemoteState, std::nullopt};
        }
        const auto parsed = parseUnsigned(result.value.text);
        return parsed.has_value()
            ? std::pair{StoreStatus::Ok, parsed}
            : std::pair{StoreStatus::InvalidRemoteState,
                        std::optional<std::uint64_t>{}};
    };

    auto [status, revision] = parseRevisionKey(keys_.revision);
    if (status != StoreStatus::Ok) {
        output.status = status;
        return output;
    }
    output.revision = revision;
    auto [published_status, published] =
        parseRevisionKey(keys_.published_revision);
    output.status = published_status;
    output.published_revision = published;
    return output;
}

StoreResult BlacklistRedisStore::rebuildFromMysql(
    const base::ArrayView<const risk::BlacklistEntry> entries,
    const RedisDeadline deadline
) {
    const std::vector<std::string> watch{
        "WATCH", keys_.cache_ready, keys_.revision,
        keys_.users, keys_.ips, keys_.devices};
    const auto watched = run(*executor_, watch, deadline);
    auto status = commandStatus(watched);
    if (status != StoreStatus::Ok || !statusIs(watched, "OK")) {
        if (status == StoreStatus::Ok) {
            executor_->invalidate();
        }
        return storeResult(
            status == StoreStatus::Ok ? StoreStatus::ProtocolError : status);
    }
    RedisStateGuard state_guard(*executor_);

    for (const auto& [key, expected] : std::array{
             std::pair{std::cref(keys_.cache_ready), "string"},
             std::pair{std::cref(keys_.revision), "string"},
             std::pair{std::cref(keys_.users), "hash"},
             std::pair{std::cref(keys_.ips), "hash"},
             std::pair{std::cref(keys_.devices), "hash"},
         }) {
        const auto checked = ensureType(
            *executor_, key.get(), expected, true, deadline);
        if (checked != StoreStatus::Ok) {
            return storeResult(leaveWatch(
                *executor_, state_guard, deadline, checked));
        }
    }

    const auto ready = run(
        *executor_, {"GET", keys_.cache_ready}, deadline);
    status = commandStatus(ready);
    if (status != StoreStatus::Ok) {
        return storeResult(leaveWatch(
            *executor_, state_guard, deadline, status));
    }
    if (ready.value.kind == RedisValueKind::String &&
        ready.value.text == "1") {
        return storeResult(leaveWatch(
            *executor_, state_guard, deadline,
            StoreStatus::WatchConflict));
    }
    if (ready.value.kind != RedisValueKind::Nil) {
        return storeResult(leaveWatch(
            *executor_, state_guard, deadline,
            StoreStatus::InvalidRemoteState));
    }

    std::uint64_t previous_revision = 0;
    const auto revision = run(
        *executor_, {"GET", keys_.revision}, deadline);
    status = commandStatus(revision);
    if (status != StoreStatus::Ok) {
        return storeResult(leaveWatch(
            *executor_, state_guard, deadline, status));
    }
    if (revision.value.kind == RedisValueKind::String) {
        const auto parsed = parseUnsigned(
            revision.value.text, kMaxRedisInteger - 1);
        if (!parsed.has_value()) {
            return storeResult(leaveWatch(
                *executor_, state_guard, deadline,
                StoreStatus::InvalidRemoteState));
        }
        previous_revision = *parsed;
    } else if (revision.value.kind != RedisValueKind::Nil) {
        return storeResult(leaveWatch(
            *executor_, state_guard, deadline,
            StoreStatus::InvalidRemoteState));
    }

    std::array<std::vector<std::pair<std::string, std::uint64_t>>, 3> grouped;
    for (const auto& entry : entries) {
        std::size_t group = 0;
        switch (entry.type) {
        case risk::EntityType::User:
            group = 0;
            break;
        case risk::EntityType::Ip: {
            group = 1;
            const auto address = domain::IpAddress::parse(entry.id);
            if (!address.has_value() ||
                address->canonicalText() != entry.id) {
                return storeResult(leaveWatch(
                    *executor_, state_guard, deadline,
                    StoreStatus::ProtocolError));
            }
            break;
        }
        case risk::EntityType::Device:
            group = 2;
            break;
        default:
            return storeResult(leaveWatch(
                *executor_, state_guard, deadline,
                StoreStatus::ProtocolError));
        }
        if (entry.id.empty() ||
            entry.id.size() > risk::BlacklistMutation::kMaxEntityIdBytes ||
            entry.id.find('\0') != std::string::npos ||
            entry.expire_at_ms > risk::BlacklistMutation::kMaxExpireAtMs) {
            return storeResult(leaveWatch(
                *executor_, state_guard, deadline,
                StoreStatus::ProtocolError));
        }
        grouped[group].emplace_back(entry.id, entry.expire_at_ms);
    }

    const auto multi = run(*executor_, {"MULTI"}, deadline);
    status = commandStatus(multi);
    if (status != StoreStatus::Ok || !statusIs(multi, "OK")) {
        const auto original = status == StoreStatus::Ok
            ? StoreStatus::ProtocolError
            : status;
        return storeResult(leaveTransaction(
            *executor_, state_guard, deadline, original));
    }

    std::size_t expected_results = 0;
    const auto queueOrDiscard = [&](const std::vector<std::string>& argv) {
        const auto queued = queueCommand(*executor_, argv, deadline);
        if (queued != StoreStatus::Ok) {
            return leaveTransaction(
                *executor_, state_guard, deadline, queued);
        }
        ++expected_results;
        return StoreStatus::Ok;
    };
    status = queueOrDiscard({
        "DEL", keys_.users, keys_.ips, keys_.devices});
    const std::array<std::reference_wrapper<const std::string>, 3> hash_keys{
        keys_.users, keys_.ips, keys_.devices};
    for (std::size_t group = 0;
         status == StoreStatus::Ok && group < grouped.size(); ++group) {
        if (grouped[group].empty()) {
            continue;
        }
        std::vector<std::string> hset{"HSET", hash_keys[group].get()};
        for (const auto& [id, expiry] : grouped[group]) {
            hset.push_back(id);
            hset.push_back(std::to_string(expiry));
        }
        status = queueOrDiscard(hset);
    }
    if (status == StoreStatus::Ok) {
        status = queueOrDiscard({"INCR", keys_.revision});
    }
    if (status == StoreStatus::Ok) {
        status = queueOrDiscard({"SET", keys_.cache_ready, "1"});
    }
    if (status != StoreStatus::Ok) {
        return storeResult(status);
    }

    const auto executed = run(*executor_, {"EXEC"}, deadline);
    status = checkedExecStatus(*executor_, state_guard, executed);
    if (status != StoreStatus::Ok) {
        return storeResult(status);
    }
    if (executed.value.elements.size() != expected_results) {
        return storeResult(StoreStatus::ProtocolError);
    }
    for (std::size_t index = 0; index < executed.value.elements.size(); ++index) {
        const auto& value = executed.value.elements[index];
        if (index + 1 == executed.value.elements.size()) {
            if (value.kind != RedisValueKind::Status || value.text != "OK") {
                return storeResult(StoreStatus::ProtocolError);
            }
        } else if (index + 2 == executed.value.elements.size()) {
            if (value.kind != RedisValueKind::Integer || value.integer < 0 ||
                static_cast<std::uint64_t>(value.integer) !=
                    previous_revision + 1) {
                return storeResult(StoreStatus::ProtocolError);
            }
        } else if (value.kind != RedisValueKind::Integer ||
                   value.integer < 0 ||
                   (index == 0 && value.integer > 3)) {
            return storeResult(StoreStatus::ProtocolError);
        }
    }
    return storeResult(StoreStatus::Ok, previous_revision + 1);
}

}  // 命名空间 aegisflow::storage
