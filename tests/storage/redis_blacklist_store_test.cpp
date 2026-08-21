#include "aegisflow/risk/blacklist_mutation.hpp"
#include "aegisflow/storage/blacklist_redis_store.hpp"
#include "aegisflow/storage/redis_connection.hpp"

#include "tests/support/test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using aegisflow::storage::BlacklistRedisStore;
using aegisflow::storage::IRedisCommandExecutor;
using aegisflow::storage::RedisCommandResult;
using aegisflow::storage::RedisCommandStatus;
using aegisflow::storage::RedisConfig;
using aegisflow::storage::RedisConnection;
using aegisflow::storage::RedisDeadline;
using aegisflow::storage::RedisKeySet;
using aegisflow::storage::RedisValue;
using aegisflow::storage::RedisValueKind;
using aegisflow::storage::StoreStatus;
using aegisflow::test::require;

RedisValue status(std::string text) {
    RedisValue value;
    value.kind = RedisValueKind::Status;
    value.text = std::move(text);
    return value;
}
RedisValue string(std::string text) {
    RedisValue value;
    value.kind = RedisValueKind::String;
    value.text = std::move(text);
    return value;
}
RedisValue integer(const std::int64_t value) {
    RedisValue result;
    result.kind = RedisValueKind::Integer;
    result.integer = value;
    return result;
}
RedisValue nil() {
    RedisValue value;
    value.kind = RedisValueKind::Nil;
    return value;
}
RedisValue error(std::string text) {
    RedisValue value;
    value.kind = RedisValueKind::Error;
    value.text = std::move(text);
    return value;
}
RedisValue array(std::vector<RedisValue> values) {
    RedisValue value;
    value.kind = RedisValueKind::Array;
    value.elements = std::move(values);
    return value;
}
RedisCommandResult ok(RedisValue value) {
    RedisCommandResult result;
    result.status = RedisCommandStatus::Ok;
    result.value = std::move(value);
    return result;
}
RedisCommandResult commandFailure(const RedisCommandStatus status) {
    RedisCommandResult result;
    result.status = status;
    return result;
}

class ScriptedExecutor final : public IRedisCommandExecutor {
public:
    struct Step {
        std::vector<std::string> expected;
        RedisCommandResult result;
    };

    void push(std::vector<std::string> expected, RedisCommandResult result) {
        steps_.push_back({std::move(expected), std::move(result)});
    }

    RedisCommandResult command(
        const aegisflow::base::ArrayView<const std::string> argv,
        RedisDeadline
    ) override {
        if (steps_.empty()) {
            throw std::runtime_error("script received unexpected command");
        }
        Step step = std::move(steps_.front());
        steps_.pop_front();
        const std::vector<std::string> actual(argv.begin(), argv.end());
        if (!(actual == step.expected)) {
            throw std::runtime_error("script command argv mismatch");
        }
        return std::move(step.result);
    }

    void invalidate() noexcept override { invalidated_ = true; }

    void requireEmpty() const {
        require(steps_.empty(), "script still contains unused replies");
    }

    [[nodiscard]] bool invalidated() const noexcept { return invalidated_; }

private:
    std::deque<Step> steps_;
    bool invalidated_ = false;
};

RedisDeadline deadline() {
    return std::chrono::steady_clock::now() + std::chrono::seconds(5);
}

void pushReadyPrecondition(
    ScriptedExecutor& script,
    const RedisKeySet& keys,
    const std::string& hash,
    std::string revision = "0"
) {
    script.push(
        {"WATCH", keys.cache_ready, keys.pending, keys.revision, hash},
        ok(status("OK")));
    script.push({"TYPE", keys.cache_ready}, ok(status("string")));
    script.push({"GET", keys.cache_ready}, ok(string("1")));
    script.push({"TYPE", keys.pending}, ok(status("stream")));
    script.push({"TYPE", keys.revision}, ok(status("string")));
    script.push({"TYPE", hash}, ok(status("hash")));
    script.push({"GET", keys.revision}, ok(string(std::move(revision))));
}

void eightKeysAreDerivedWithoutAliases() {
    const auto keys = RedisKeySet::fromPrefix("unit:blacklist");
    require(keys.users == "unit:blacklist:user", "user key error");
    require(keys.ips == "unit:blacklist:ip", "ip key error");
    require(keys.devices == "unit:blacklist:device", "device key error");
    require(keys.pending == "unit:blacklist:pending", "pending key error");
    require(keys.revision == "unit:blacklist:revision", "revision key error");
    require(keys.cache_ready == "unit:blacklist:cache_ready", "ready key error");
    require(keys.published_revision == "unit:blacklist:published_revision",
            "published key error");
    require(keys.reset_barrier == "unit:blacklist:reset_barrier",
            "barrier key error");
    require(keys.all().size() == 8, "key set must contain exactly eight keys");
}

void readyUpsertValidatesAndCommitsWholeReply() {
    ScriptedExecutor script;
    const auto keys = RedisKeySet::fromPrefix("unit");
    script.push(
        {"WATCH", keys.cache_ready, keys.pending, keys.revision, keys.ips},
        ok(status("OK")));
    script.push({"TYPE", keys.cache_ready}, ok(status("string")));
    script.push({"GET", keys.cache_ready}, ok(string("1")));
    script.push({"TYPE", keys.pending}, ok(status("stream")));
    script.push({"TYPE", keys.revision}, ok(status("string")));
    script.push({"TYPE", keys.ips}, ok(status("hash")));
    script.push({"GET", keys.revision}, ok(string("7")));
    script.push({"MULTI"}, ok(status("OK")));
    script.push(
        {"HSET", keys.ips, "2001:db8::7", "9000"},
        ok(status("QUEUED")));
    script.push(
        {"XADD", keys.pending, "*", "operation", "UPSERT", "type", "IP",
         "id", "2001:db8::7", "reason", "manual", "expire_at_ms", "9000"},
        ok(status("QUEUED")));
    script.push({"INCR", keys.revision}, ok(status("QUEUED")));
    script.push(
        {"EXEC"},
        ok(array({integer(1), string("1-0"), integer(8)})));

    const auto mutation = aegisflow::risk::BlacklistMutation::upsert(
        aegisflow::risk::EntityType::Ip,
        "2001:0db8:0:0:0:0:0:7",
        "manual",
        9000);
    require(mutation.has_value(), "mutation setup failed");
    BlacklistRedisStore store(script, keys);
    const std::vector mutations{*mutation};
    const auto result = store.applyMutations(mutations, deadline());
    require(result.status == StoreStatus::Ok && result.revision == 8,
            "valid transaction must publish next revision");
    script.requireEmpty();
}

void missingReadyStopsBeforeTransaction() {
    ScriptedExecutor script;
    const auto keys = RedisKeySet::fromPrefix("cold");
    script.push(
        {"WATCH", keys.cache_ready, keys.pending, keys.revision, keys.users},
        ok(status("OK")));
    script.push({"TYPE", keys.cache_ready}, ok(status("none")));
    script.push({"GET", keys.cache_ready}, ok(nil()));
    script.push({"UNWATCH"}, ok(status("OK")));

    const auto mutation = aegisflow::risk::BlacklistMutation::disable(
        aegisflow::risk::EntityType::User, "user-7");
    require(mutation.has_value(), "mutation setup failed");
    BlacklistRedisStore store(script, keys);
    const std::vector mutations{*mutation};
    require(store.applyMutations(mutations, deadline()).status ==
                StoreStatus::NotReady,
            "missing ready marker must reject management write");
    script.requireEmpty();
}

void execOutcomesRemainDistinguishable() {
    const auto runCase = [](RedisCommandResult exec_reply,
                            const StoreStatus expected) {
        ScriptedExecutor script;
        const auto keys = RedisKeySet::fromPrefix("exec");
        script.push(
            {"WATCH", keys.cache_ready, keys.pending, keys.revision, keys.users},
            ok(status("OK")));
        script.push({"TYPE", keys.cache_ready}, ok(status("string")));
        script.push({"GET", keys.cache_ready}, ok(string("1")));
        script.push({"TYPE", keys.pending}, ok(status("stream")));
        script.push({"TYPE", keys.revision}, ok(status("string")));
        script.push({"TYPE", keys.users}, ok(status("hash")));
        script.push({"GET", keys.revision}, ok(string("0")));
        script.push({"MULTI"}, ok(status("OK")));
        script.push(
            {"HDEL", keys.users, "user"}, ok(status("QUEUED")));
        script.push(
            {"XADD", keys.pending, "*", "operation", "DISABLE", "type",
             "USER", "id", "user"},
            ok(status("QUEUED")));
        script.push({"INCR", keys.revision}, ok(status("QUEUED")));
        script.push({"EXEC"}, std::move(exec_reply));

        const auto mutation = aegisflow::risk::BlacklistMutation::disable(
            aegisflow::risk::EntityType::User, "user");
        BlacklistRedisStore store(script, keys);
        const std::vector mutations{*mutation};
        require(store.applyMutations(mutations, deadline()).status == expected,
                "EXEC outcome status mismatch");
        script.requireEmpty();
    };

    runCase(ok(nil()), StoreStatus::WatchConflict);
    runCase(
        commandFailure(RedisCommandStatus::IoError),
        StoreStatus::CommitUnknown);
    runCase(ok(error("EXECABORT")), StoreStatus::TransactionError);
    runCase(
        ok(array({integer(0), error("WRONGTYPE"), integer(1)})),
        StoreStatus::TransactionError);
    runCase(ok(array({integer(0), string("1-0")})),
            StoreStatus::ProtocolError);
}

void remoteTypesRevisionsAndQueueRepliesAreStrict() {
    const auto mutation = aegisflow::risk::BlacklistMutation::disable(
        aegisflow::risk::EntityType::User, "user");
    require(mutation.has_value(), "mutation setup failed");

    {
        ScriptedExecutor script;
        const auto keys = RedisKeySet::fromPrefix("wrong-type");
        script.push(
            {"WATCH", keys.cache_ready, keys.pending, keys.revision,
             keys.users},
            ok(status("OK")));
        script.push({"TYPE", keys.cache_ready}, ok(status("hash")));
        script.push({"UNWATCH"}, ok(status("OK")));
        BlacklistRedisStore store(script, keys);
        const std::vector mutations{*mutation};
        require(store.applyMutations(mutations, deadline()).status ==
                    StoreStatus::InvalidRemoteState,
                "wrong ready TYPE must stop before MULTI");
        script.requireEmpty();
    }

    for (const std::string revision : {
             std::string("-1"), std::string("01"), std::string("abc"),
             std::to_string(std::numeric_limits<std::int64_t>::max())}) {
        ScriptedExecutor script;
        const auto keys = RedisKeySet::fromPrefix("bad-revision");
        pushReadyPrecondition(script, keys, keys.users, revision);
        script.push({"UNWATCH"}, ok(status("OK")));
        BlacklistRedisStore store(script, keys);
        const std::vector mutations{*mutation};
        require(store.applyMutations(mutations, deadline()).status ==
                    StoreStatus::InvalidRemoteState,
                "non-incrementable revision must be rejected");
        script.requireEmpty();
    }

    {
        ScriptedExecutor script;
        const auto keys = RedisKeySet::fromPrefix("queue");
        pushReadyPrecondition(script, keys, keys.users);
        script.push({"MULTI"}, ok(status("OK")));
        script.push({"HDEL", keys.users, "user"}, ok(status("NO")));
        script.push({"DISCARD"}, ok(status("OK")));
        BlacklistRedisStore store(script, keys);
        const std::vector mutations{*mutation};
        require(store.applyMutations(mutations, deadline()).status ==
                    StoreStatus::ProtocolError,
                "non-QUEUED reply must discard transaction");
        script.requireEmpty();
    }

    {
        ScriptedExecutor script;
        const auto keys = RedisKeySet::fromPrefix("revision-mismatch");
        pushReadyPrecondition(script, keys, keys.users, "7");
        script.push({"MULTI"}, ok(status("OK")));
        script.push({"HDEL", keys.users, "user"}, ok(status("QUEUED")));
        script.push(
            {"XADD", keys.pending, "*", "operation", "DISABLE", "type",
             "USER", "id", "user"},
            ok(status("QUEUED")));
        script.push({"INCR", keys.revision}, ok(status("QUEUED")));
        script.push(
            {"EXEC"}, ok(array({integer(0), string("1-0"), integer(9)})));
        BlacklistRedisStore store(script, keys);
        const std::vector mutations{*mutation};
        require(store.applyMutations(mutations, deadline()).status ==
                    StoreStatus::ProtocolError,
                "EXEC revision must equal precondition plus one");
        script.requireEmpty();
    }
}

void failedTransactionCleanupInvalidatesExecutor() {
    const auto mutation = aegisflow::risk::BlacklistMutation::disable(
        aegisflow::risk::EntityType::User, "user");
    const auto keys = RedisKeySet::fromPrefix("invalidate");
    ScriptedExecutor script;
    pushReadyPrecondition(script, keys, keys.users);
    script.push({"MULTI"}, ok(status("OK")));
    script.push({"HDEL", keys.users, "user"}, ok(status("NOT-QUEUED")));
    script.push(
        {"DISCARD"},
        commandFailure(RedisCommandStatus::DeadlineExceeded));
    BlacklistRedisStore store(script, keys);
    const std::vector mutations{*mutation};
    require(store.applyMutations(mutations, deadline()).status ==
                StoreStatus::DeadlineExceeded,
            "cleanup deadline must be reported");
    require(script.invalidated(),
            "uncertain MULTI cleanup must invalidate executor");
    script.requireEmpty();
}

void clearAllQueuesOneOrderedTransaction() {
    ScriptedExecutor script;
    const auto keys = RedisKeySet::fromPrefix("clear");
    script.push(
        {"WATCH", keys.cache_ready, keys.pending, keys.revision,
         keys.devices, keys.ips, keys.users},
        ok(status("OK")));
    script.push({"TYPE", keys.cache_ready}, ok(status("string")));
    script.push({"GET", keys.cache_ready}, ok(string("1")));
    script.push({"TYPE", keys.pending}, ok(status("stream")));
    script.push({"TYPE", keys.revision}, ok(status("string")));
    script.push({"TYPE", keys.devices}, ok(status("hash")));
    script.push({"TYPE", keys.ips}, ok(status("hash")));
    script.push({"TYPE", keys.users}, ok(status("hash")));
    script.push({"GET", keys.revision}, ok(string("12")));
    script.push({"MULTI"}, ok(status("OK")));
    script.push(
        {"DEL", keys.users, keys.ips, keys.devices},
        ok(status("QUEUED")));
    script.push(
        {"XADD", keys.pending, "*", "operation", "CLEAR_ALL"},
        ok(status("QUEUED")));
    script.push({"INCR", keys.revision}, ok(status("QUEUED")));
    script.push(
        {"EXEC"}, ok(array({integer(3), string("13-0"), integer(13)})));
    BlacklistRedisStore store(script, keys);
    const std::vector mutations{
        aegisflow::risk::BlacklistMutation::clearAll()};
    const auto result = store.applyMutations(mutations, deadline());
    require(result.status == StoreStatus::Ok && result.revision == 13,
            "CLEAR_ALL must atomically DEL/XADD/INCR");
    script.requireEmpty();
}

void rebuildValidatesEntriesAndPublishesReadyLast() {
    const auto keys = RedisKeySet::fromPrefix("rebuild");
    {
        ScriptedExecutor script;
        script.push(
            {"WATCH", keys.cache_ready, keys.revision, keys.users, keys.ips,
             keys.devices},
            ok(status("OK")));
        for (const auto& [key, type] : std::vector<std::pair<std::string, std::string>>{
                 {keys.cache_ready, "none"}, {keys.revision, "none"},
                 {keys.users, "none"}, {keys.ips, "none"},
                 {keys.devices, "none"}}) {
            script.push({"TYPE", key}, ok(status(type)));
        }
        script.push({"GET", keys.cache_ready}, ok(nil()));
        script.push({"GET", keys.revision}, ok(nil()));
        script.push({"MULTI"}, ok(status("OK")));
        script.push(
            {"DEL", keys.users, keys.ips, keys.devices},
            ok(status("QUEUED")));
        script.push(
            {"HSET", keys.users, "user", "0"}, ok(status("QUEUED")));
        script.push(
            {"HSET", keys.ips, "192.0.2.8", "8000"},
            ok(status("QUEUED")));
        script.push(
            {"HSET", keys.devices, "device", "9000"},
            ok(status("QUEUED")));
        script.push({"INCR", keys.revision}, ok(status("QUEUED")));
        script.push(
            {"SET", keys.cache_ready, "1"}, ok(status("QUEUED")));
        script.push(
            {"EXEC"},
            ok(array({integer(0), integer(1), integer(1), integer(1),
                      integer(1), status("OK")})));
        BlacklistRedisStore store(script, keys);
        const std::vector<aegisflow::risk::BlacklistEntry> entries{
            {aegisflow::risk::EntityType::User, "user", 0},
            {aegisflow::risk::EntityType::Ip, "192.0.2.8", 8000},
            {aegisflow::risk::EntityType::Device, "device", 9000},
        };
        const auto result = store.rebuildFromMysql(entries, deadline());
        require(result.status == StoreStatus::Ok && result.revision == 1,
                "missing revision must rebuild to revision one");
        script.requireEmpty();
    }

    const auto rejectEntry = [&](aegisflow::risk::BlacklistEntry entry) {
        ScriptedExecutor script;
        script.push(
            {"WATCH", keys.cache_ready, keys.revision, keys.users, keys.ips,
             keys.devices},
            ok(status("OK")));
        for (const auto& key :
             {keys.cache_ready, keys.revision, keys.users, keys.ips,
              keys.devices}) {
            script.push({"TYPE", key}, ok(status("none")));
        }
        script.push({"GET", keys.cache_ready}, ok(nil()));
        script.push({"GET", keys.revision}, ok(nil()));
        script.push({"UNWATCH"}, ok(status("OK")));
        BlacklistRedisStore store(script, keys);
        const std::vector entries{std::move(entry)};
        require(store.rebuildFromMysql(entries, deadline()).status ==
                    StoreStatus::ProtocolError,
                "invalid MySQL candidate must not enter MULTI");
        script.requireEmpty();
    };
    rejectEntry({static_cast<aegisflow::risk::EntityType>(99), "bad", 0});
    rejectEntry({aegisflow::risk::EntityType::User, std::string(129, 'u'), 0});

    {
        ScriptedExecutor script;
        script.push(
            {"WATCH", keys.cache_ready, keys.revision, keys.users, keys.ips,
             keys.devices},
            ok(status("OK")));
        for (const auto& key :
             {keys.cache_ready, keys.revision, keys.users, keys.ips,
              keys.devices}) {
            script.push({"TYPE", key}, ok(status("none")));
        }
        script.push({"GET", keys.cache_ready}, ok(nil()));
        script.push({"GET", keys.revision}, ok(string("bad")));
        script.push({"UNWATCH"}, ok(status("OK")));
        BlacklistRedisStore store(script, keys);
        const std::vector<aegisflow::risk::BlacklistEntry> no_entries;
        require(store.rebuildFromMysql(no_entries, deadline()).status ==
                    StoreStatus::InvalidRemoteState,
                "bad cold revision must stop before MULTI");
        script.requireEmpty();
    }
}

void publishedRevisionAndResetBarrierAreTyped() {
    ScriptedExecutor script;
    const auto keys = RedisKeySet::fromPrefix("control");
    script.push(
        {"WATCH", keys.published_revision}, ok(status("OK")));
    script.push({"TYPE", keys.published_revision}, ok(status("none")));
    script.push({"MULTI"}, ok(status("OK")));
    script.push(
        {"SET", keys.published_revision, "7"}, ok(status("QUEUED")));
    script.push({"EXEC"}, ok(array({status("OK")})));
    script.push(
        {"HINCRBY", keys.reset_barrier, "requested", "1"},
        ok(integer(1)));
    script.push(
        {"HMGET", keys.reset_barrier, "requested", "completed"},
        ok(array({string("1"), nil()})));
    script.push({"WATCH", keys.reset_barrier}, ok(status("OK")));
    script.push(
        {"HMGET", keys.reset_barrier, "requested", "completed"},
        ok(array({string("1"), nil()})));
    script.push({"MULTI"}, ok(status("OK")));
    script.push(
        {"HSET", keys.reset_barrier, "completed", "1"},
        ok(status("QUEUED")));
    script.push({"EXEC"}, ok(array({integer(1)})));
    script.push({"HLEN", keys.users}, ok(integer(0)));
    script.push({"HLEN", keys.ips}, ok(integer(0)));
    script.push({"HLEN", keys.devices}, ok(integer(0)));
    script.push({"XLEN", keys.pending}, ok(integer(0)));
    script.push({"GET", keys.revision}, ok(string("7")));
    script.push({"GET", keys.published_revision}, ok(string("7")));

    BlacklistRedisStore store(script, keys);
    require(store.setPublishedRevision(7, deadline()) == StoreStatus::Ok,
            "published revision transaction failed");
    const auto requested = store.requestResetBarrier(deadline());
    require(requested.status == StoreStatus::Ok && requested.revision == 1,
            "reset token allocation failed");
    const auto state = store.readResetBarrier(deadline());
    require(state.status == StoreStatus::Ok && state.state.requested == 1 &&
                state.state.completed == 0,
            "reset state parsing failed");
    require(store.completeResetBarrier(1, deadline()) == StoreStatus::Ok,
            "monotonic reset completion failed");
    const auto convergence = store.readResetConvergence(deadline());
    require(convergence.redisAndPublicationEmpty(),
            "four Redis/publication reset predicates must converge");
    script.requireEmpty();
}

RedisValue streamEntry(
    std::string id,
    std::vector<RedisValue> fields
) {
    return array({string(std::move(id)), array(std::move(fields))});
}

void streamParsingPreservesDeletableBadEntries() {
    ScriptedExecutor script;
    const auto keys = RedisKeySet::fromPrefix("stream");
    script.push(
        {"XRANGE", keys.pending, "-", "+", "COUNT", "4"},
        ok(array({
            streamEntry(
                "1-0",
                {string("operation"), string("UPSERT"), string("type"),
                 string("IP"), string("id"), string("2001:db8::1"),
                 string("reason"), string("manual"),
                 string("expire_at_ms"), string("9000")}),
            streamEntry(
                "2-0",
                {string("operation"), string("CLEAR_ALL"),
                 string("type"), string("USER")}),
        })));
    BlacklistRedisStore store(script, keys);
    const auto result = store.readPending(4, deadline());
    require(result.status == StoreStatus::Ok && result.entries.size() == 2,
            "well-shaped stream array must be returned");
    require(result.entries[0].mutation.has_value() &&
                result.entries[0].mutation->id() == "2001:db8::1",
            "valid stream mutation must parse through value factory");
    require(!result.entries[1].mutation.has_value() &&
                !result.entries[1].error.empty() &&
                result.entries[1].stream_id == "2-0",
            "bad fields must retain a valid stream id for XDEL");
    script.requireEmpty();

    ScriptedExecutor bad_id;
    bad_id.push(
        {"XRANGE", keys.pending, "-", "+", "COUNT", "1"},
        ok(array({streamEntry(
            "",
            {string("operation"), string("CLEAR_ALL")})})));
    BlacklistRedisStore bad_store(bad_id, keys);
    require(bad_store.readPending(1, deadline()).status ==
                StoreStatus::ProtocolError,
            "empty stream id is a reply protocol failure");
    bad_id.requireEmpty();
}

void stableScanRejectsMixedRevision() {
    ScriptedExecutor script;
    const auto keys = RedisKeySet::fromPrefix("scan");
    script.push({"GET", keys.revision}, ok(string("4")));
    script.push(
        {"HSCAN", keys.users, "0", "COUNT", "8"},
        ok(array({string("0"), array({string("user"), string("0")})})));
    script.push(
        {"HSCAN", keys.ips, "0", "COUNT", "8"},
        ok(array({string("0"), array({})})));
    script.push(
        {"HSCAN", keys.devices, "0", "COUNT", "8"},
        ok(array({string("0"), array({})})));
    script.push({"GET", keys.revision}, ok(string("5")));
    BlacklistRedisStore store(script, keys);
    require(store.loadStableSnapshot(8, deadline()).status ==
                StoreStatus::WatchConflict,
            "scan spanning two revisions must never be published");
    script.requireEmpty();
}

void expiredFieldsDeleteOnlyAtTheScannedRevisionAndValue() {
    ScriptedExecutor script;
    const auto keys = RedisKeySet::fromPrefix("expire");
    script.push(
        {"WATCH", keys.revision, keys.users, keys.ips},
        ok(status("OK")));
    script.push({"TYPE", keys.revision}, ok(status("string")));
    script.push({"TYPE", keys.users}, ok(status("hash")));
    script.push({"TYPE", keys.ips}, ok(status("hash")));
    script.push({"GET", keys.revision}, ok(string("7")));
    script.push({"HGET", keys.users, "expired-user"}, ok(string("100")));
    script.push({"HGET", keys.ips, "2001:db8::7"}, ok(string("200")));
    script.push({"MULTI"}, ok(status("OK")));
    script.push(
        {"HDEL", keys.users, "expired-user"}, ok(status("QUEUED")));
    script.push(
        {"HDEL", keys.ips, "2001:db8::7"}, ok(status("QUEUED")));
    script.push({"INCR", keys.revision}, ok(status("QUEUED")));
    script.push(
        {"EXEC"}, ok(array({integer(1), integer(1), integer(8)})));

    BlacklistRedisStore store(script, keys);
    const std::vector<aegisflow::risk::BlacklistEntry> expired{
        {aegisflow::risk::EntityType::User, "expired-user", 100},
        {aegisflow::risk::EntityType::Ip, "2001:db8::7", 200},
    };
    const auto result = store.removeExpiredFields(
        expired, 7, 200, deadline());
    require(result.status == StoreStatus::Ok && result.revision == 8,
            "过期字段必须在同一事务中删除并递增 revision");
    script.requireEmpty();
}

void renewedOrDeletedFieldsAbortTheWholeExpiryBatch() {
    const auto runChangedField = [](RedisValue current_value) {
        ScriptedExecutor script;
        const auto keys = RedisKeySet::fromPrefix("renewed");
        script.push(
            {"WATCH", keys.revision, keys.ips}, ok(status("OK")));
        script.push({"TYPE", keys.revision}, ok(status("string")));
        script.push({"TYPE", keys.ips}, ok(status("hash")));
        script.push({"GET", keys.revision}, ok(string("7")));
        script.push(
            {"HGET", keys.ips, "192.0.2.7"}, ok(std::move(current_value)));
        script.push({"UNWATCH"}, ok(status("OK")));

        BlacklistRedisStore store(script, keys);
        const std::vector<aegisflow::risk::BlacklistEntry> expired{
            {aegisflow::risk::EntityType::Ip, "192.0.2.7", 100},
        };
        require(store.removeExpiredFields(expired, 7, 200, deadline()).status ==
                    StoreStatus::WatchConflict,
                "续期或已删除 field 必须放弃整批过期删除");
        script.requireEmpty();
    };

    runChangedField(string("300"));
    runChangedField(nil());
}

void expiryCleanupRejectsChangedRevisionBeforeMulti() {
    ScriptedExecutor script;
    const auto keys = RedisKeySet::fromPrefix("expiry-revision");
    script.push(
        {"WATCH", keys.revision, keys.users}, ok(status("OK")));
    script.push({"TYPE", keys.revision}, ok(status("string")));
    script.push({"TYPE", keys.users}, ok(status("hash")));
    script.push({"GET", keys.revision}, ok(string("8")));
    script.push({"UNWATCH"}, ok(status("OK")));

    BlacklistRedisStore store(script, keys);
    const std::vector<aegisflow::risk::BlacklistEntry> expired{
        {aegisflow::risk::EntityType::User, "expired-user", 100},
    };
    require(store.removeExpiredFields(expired, 7, 200, deadline()).status ==
                StoreStatus::WatchConflict,
            "扫描后 revision 变化必须在 MULTI 前放弃");
    script.requireEmpty();
}

void expiryCleanupRejectsNonExpiringFutureAndIllegalEntries() {
    const auto keys = RedisKeySet::fromPrefix("bad-expiry");
    const auto reject = [&](aegisflow::risk::BlacklistEntry entry) {
        ScriptedExecutor script;
        BlacklistRedisStore store(script, keys);
        const std::vector entries{std::move(entry)};
        require(store.removeExpiredFields(entries, 7, 200, deadline()).status ==
                    StoreStatus::ProtocolError,
                "非过期或非法 field 不得进入 WATCH");
        script.requireEmpty();
    };

    reject({aegisflow::risk::EntityType::User, "forever", 0});
    reject({aegisflow::risk::EntityType::User, "future", 201});
    reject({aegisflow::risk::EntityType::Ip, "2001:0db8::7", 100});
    reject({static_cast<aegisflow::risk::EntityType>(99), "bad", 100});

    ScriptedExecutor script;
    BlacklistRedisStore store(script, keys);
    const std::vector<aegisflow::risk::BlacklistEntry> no_entries;
    require(store.removeExpiredFields(no_entries, 7, 200, deadline()).status ==
                StoreStatus::ProtocolError,
            "空过期批次不得开启事务");
    script.requireEmpty();
}

void expiryCleanupReportsExecErrorsWithoutPublishingRevision() {
    ScriptedExecutor script;
    const auto keys = RedisKeySet::fromPrefix("expiry-exec");
    script.push(
        {"WATCH", keys.revision, keys.devices}, ok(status("OK")));
    script.push({"TYPE", keys.revision}, ok(status("string")));
    script.push({"TYPE", keys.devices}, ok(status("hash")));
    script.push({"GET", keys.revision}, ok(string("7")));
    script.push({"HGET", keys.devices, "device-7"}, ok(string("100")));
    script.push({"MULTI"}, ok(status("OK")));
    script.push(
        {"HDEL", keys.devices, "device-7"}, ok(status("QUEUED")));
    script.push({"INCR", keys.revision}, ok(status("QUEUED")));
    script.push(
        {"EXEC"}, ok(array({error("WRONGTYPE"), integer(8)})));

    BlacklistRedisStore store(script, keys);
    const std::vector<aegisflow::risk::BlacklistEntry> expired{
        {aegisflow::risk::EntityType::Device, "device-7", 100},
    };
    const auto result = store.removeExpiredFields(
        expired, 7, 200, deadline());
    require(result.status == StoreStatus::TransactionError &&
                !result.revision.has_value(),
            "EXEC 内嵌错误不得返回已发布 revision");
    script.requireEmpty();
}

std::optional<RedisConfig> redisIntegrationConfig() {
    const char* host = std::getenv("AEGISFLOW_TEST_REDIS_HOST");
    const char* port = std::getenv("AEGISFLOW_TEST_REDIS_PORT");
    if (host == nullptr || *host == '\0' || port == nullptr || *port == '\0') {
        return std::nullopt;
    }
    std::size_t parsed = 0;
    const auto port_number = std::stoul(port, &parsed);
    if (parsed != std::string_view(port).size() || port_number == 0 ||
        port_number > 65'535) {
        throw std::runtime_error("AEGISFLOW_TEST_REDIS_PORT invalid");
    }
    RedisConfig config;
    config.host = host;
    config.port = static_cast<std::uint16_t>(port_number);
    if (const char* username = std::getenv("AEGISFLOW_TEST_REDIS_USERNAME")) {
        config.username = username;
    }
    if (const char* password = std::getenv("AEGISFLOW_TEST_REDIS_PASSWORD")) {
        config.password = password;
    }
    if (const char* database = std::getenv("AEGISFLOW_TEST_REDIS_DATABASE")) {
        std::size_t db_parsed = 0;
        const auto db = std::stoul(database, &db_parsed);
        if (db_parsed != std::string_view(database).size() || db > 15) {
            throw std::runtime_error("AEGISFLOW_TEST_REDIS_DATABASE invalid");
        }
        config.database = static_cast<std::uint32_t>(db);
    }
    return config;
}

RedisCommandResult command(
    IRedisCommandExecutor& executor,
    std::vector<std::string> argv
) {
    return executor.command(argv, deadline());
}

void requireIntegerReply(
    const RedisCommandResult& result,
    const std::int64_t expected,
    const std::string_view message
) {
    require(result.status == RedisCommandStatus::Ok &&
                result.value.kind == RedisValueKind::Integer &&
                result.value.integer == expected,
            message);
}

class ExactKeyCleanup final {
public:
    ExactKeyCleanup(IRedisCommandExecutor& executor, RedisKeySet keys)
        : executor_(&executor), keys_(std::move(keys)) {}
    ~ExactKeyCleanup() {
        std::vector<std::string> argv{"DEL"};
        const auto all = keys_.all();
        argv.insert(argv.end(), all.begin(), all.end());
        (void)command(*executor_, std::move(argv));
    }

private:
    IRedisCommandExecutor* executor_;
    RedisKeySet keys_;
};

class ConflictExecutor final : public IRedisCommandExecutor {
public:
    ConflictExecutor(
        IRedisCommandExecutor& primary,
        IRedisCommandExecutor& competitor,
        std::string revision_key
    ) : primary_(&primary), competitor_(&competitor),
        revision_key_(std::move(revision_key)) {}

    RedisCommandResult command(
        const aegisflow::base::ArrayView<const std::string> argv,
        const RedisDeadline call_deadline
    ) override {
        if (!triggered_ && argv.size() == 1 && argv[0] == "MULTI") {
            triggered_ = true;
            const std::vector<std::string> set{
                "SET", revision_key_, "1"};
            const auto changed = competitor_->command(set, call_deadline);
            require(changed.status == RedisCommandStatus::Ok &&
                        changed.value.kind == RedisValueKind::Status &&
                        changed.value.text == "OK",
                    "competitor failed to change watched revision");
        }
        return primary_->command(argv, call_deadline);
    }

    void invalidate() noexcept override { primary_->invalidate(); }

private:
    IRedisCommandExecutor* primary_;
    IRedisCommandExecutor* competitor_;
    std::string revision_key_;
    bool triggered_ = false;
};

void realRedisTransactionConflictAndRebuild() {
    const auto config = redisIntegrationConfig();
    require(config.has_value(), "Redis integration config missing");
    auto primary = RedisConnection::connect(*config, deadline());
    auto competitor = RedisConnection::connect(*config, deadline());
    require(primary != nullptr && competitor != nullptr,
            "cannot connect with explicit Redis test config");

    const auto prefix = std::string("aegisflow:test:") +
        std::to_string(static_cast<long long>(::getpid())) + ":" +
        std::to_string(std::chrono::steady_clock::now()
                           .time_since_epoch().count());
    const auto keys = RedisKeySet::fromPrefix(prefix);
    const auto wrong_prefix = prefix + ":wrong";
    const auto wrong_keys = RedisKeySet::fromPrefix(wrong_prefix);
    std::vector<std::string> exists{"EXISTS"};
    const auto all = keys.all();
    exists.insert(exists.end(), all.begin(), all.end());
    const auto wrong_all = wrong_keys.all();
    exists.insert(exists.end(), wrong_all.begin(), wrong_all.end());
    requireIntegerReply(command(*primary, exists), 0,
                        "unique prefix unexpectedly collides");

    {
    ExactKeyCleanup cleanup(*primary, keys);

    require(command(*primary, {"SET", keys.cache_ready, "1"}).value.text ==
                "OK",
            "ready setup failed");
    require(command(*primary, {"SET", keys.revision, "0"}).value.text == "OK",
            "revision setup failed");
    BlacklistRedisStore store(*primary, keys);
    const auto upsert = aegisflow::risk::BlacklistMutation::upsert(
        aegisflow::risk::EntityType::Ip,
        "2001:0db8:0:0:0:0:0:51", "integration", 9000);
    const std::vector mutations{*upsert};
    const auto applied = store.applyMutations(mutations, deadline());
    require(applied.status == StoreStatus::Ok && applied.revision == 1,
            "real Redis transaction failed");
    requireIntegerReply(command(*primary, {"HLEN", keys.ips}), 1,
                        "Hash mutation not visible");
    requireIntegerReply(command(*primary, {"XLEN", keys.pending}), 1,
                        "Stream mutation not visible");

    const auto stable = store.loadStableSnapshot(16, deadline());
    require(stable.status == StoreStatus::Ok && stable.revision == 1 &&
                stable.entries.size() == 1 &&
                stable.entries.front().id == "2001:db8::51",
            "stable HSCAN did not return canonical snapshot");
    const auto expired = store.removeExpiredFields(
        stable.entries, stable.revision, 10'000, deadline());
    require(expired.status == StoreStatus::Ok && expired.revision == 2,
            "real Redis expiry transaction failed");
    requireIntegerReply(command(*primary, {"HLEN", keys.ips}), 0,
                        "expired Hash field remains visible");

    require(command(*primary, {"DEL", keys.ips, keys.pending}).status ==
                RedisCommandStatus::Ok,
            "case reset failed");
    require(command(*primary, {"SET", keys.revision, "0"}).value.text == "OK",
            "conflict revision setup failed");
    ConflictExecutor conflict(*primary, *competitor, keys.revision);
    BlacklistRedisStore conflict_store(conflict, keys);
    require(conflict_store.applyMutations(mutations, deadline()).status ==
                StoreStatus::WatchConflict,
            "second connection must invalidate WATCH transaction");
    requireIntegerReply(command(*primary, {"HLEN", keys.ips}), 0,
                        "watch-conflicted hash write became visible");
    requireIntegerReply(command(*primary, {"XLEN", keys.pending}), 0,
                        "watch-conflicted stream write became visible");

    require(command(*primary, {"DEL", keys.cache_ready, keys.revision}).status ==
                RedisCommandStatus::Ok,
            "cold rebuild reset failed");
    const std::vector<aegisflow::risk::BlacklistEntry> entries{
        {aegisflow::risk::EntityType::User, "redis-user", 0},
        {aegisflow::risk::EntityType::Ip, "192.0.2.51", 10'000},
        {aegisflow::risk::EntityType::Device, "redis-device", 20'000},
    };
    const auto rebuilt = store.rebuildFromMysql(entries, deadline());
    require(rebuilt.status == StoreStatus::Ok && rebuilt.revision == 1,
            "cold rebuild transaction failed");
    const auto rebuilt_snapshot = store.loadStableSnapshot(16, deadline());
    require(rebuilt_snapshot.status == StoreStatus::Ok &&
                rebuilt_snapshot.entries.size() == 3,
            "rebuilt hashes are incomplete");

    ExactKeyCleanup wrong_cleanup(*primary, wrong_keys);
    require(command(*primary, {"SET", wrong_keys.cache_ready, "1"}).value.text ==
                "OK" &&
                command(*primary, {"SET", wrong_keys.revision, "0"}).value.text ==
                    "OK" &&
                command(*primary, {"SET", wrong_keys.users, "wrong"}).value.text ==
                    "OK",
            "wrong-type setup failed");
    BlacklistRedisStore wrong_store(*primary, wrong_keys);
    const auto disable = aegisflow::risk::BlacklistMutation::disable(
        aegisflow::risk::EntityType::User, "redis-user");
    const std::vector disables{*disable};
    require(wrong_store.applyMutations(disables, deadline()).status ==
                StoreStatus::InvalidRemoteState,
            "wrong Hash TYPE was not rejected");
    requireIntegerReply(command(*primary, {"XLEN", wrong_keys.pending}), 0,
                        "wrong TYPE caused a Stream side effect");

    // 清理使用新连接，不依赖被测 WATCH/MULTI 连接的状态。
    auto cleanup_connection = RedisConnection::connect(*config, deadline());
    require(cleanup_connection != nullptr,
            "cannot create independent cleanup connection");
    std::vector<std::string> delete_all{"DEL"};
    delete_all.insert(delete_all.end(), all.begin(), all.end());
    delete_all.insert(delete_all.end(), wrong_all.begin(), wrong_all.end());
    const auto deleted = command(*cleanup_connection, delete_all);
    require(deleted.status == RedisCommandStatus::Ok &&
                deleted.value.kind == RedisValueKind::Integer &&
                deleted.value.integer >= 0,
            "independent exact-key cleanup failed");
    requireIntegerReply(command(*cleanup_connection, exists), 0,
                        "cleanup connection still sees exact test keys");
    }
    auto verification = RedisConnection::connect(*config, deadline());
    require(verification != nullptr, "cannot verify Redis teardown");
    requireIntegerReply(command(*verification, exists), 0,
                        "new connection sees Redis test state after teardown");
}

}  // namespace

int main(const int argc, char** argv) {
    const bool integration_only =
        argc == 2 && std::string_view(argv[1]) == "--integration";
    if (argc > 2 || (argc == 2 && !integration_only)) {
        std::cerr << "usage: test_redis_blacklist_store [--integration]\n";
        return 2;
    }
    if (integration_only) {
        if (!redisIntegrationConfig().has_value()) {
            std::cout << "[SKIP] redis_blacklist_store_integration: set "
                         "AEGISFLOW_TEST_REDIS_HOST/PORT\n";
            return 0;
        }
        try {
            realRedisTransactionConflictAndRebuild();
            std::cout << "[PASS] redis_blacklist_store_integration: 1/1\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] redis_blacklist_store_integration: "
                      << error.what() << '\n';
            return 1;
        }
    }

    const int contract_result = aegisflow::test::runModule(
        "redis_blacklist_store",
        {
            {"eight typed keys", eightKeysAreDerivedWithoutAliases},
            {"watched upsert", readyUpsertValidatesAndCommitsWholeReply},
            {"ready marker", missingReadyStopsBeforeTransaction},
            {"EXEC outcomes", execOutcomesRemainDistinguishable},
            {"remote state and QUEUED", remoteTypesRevisionsAndQueueRepliesAreStrict},
            {"failed cleanup invalidates", failedTransactionCleanupInvalidatesExecutor},
            {"CLEAR_ALL transaction", clearAllQueuesOneOrderedTransaction},
            {"cold rebuild", rebuildValidatesEntriesAndPublishesReadyLast},
            {"published revision and reset", publishedRevisionAndResetBarrierAreTyped},
            {"stream parsing", streamParsingPreservesDeletableBadEntries},
            {"stable scan", stableScanRejectsMixedRevision},
            {"revision-guarded expiry delete",
             expiredFieldsDeleteOnlyAtTheScannedRevisionAndValue},
            {"renewed expiry conflict",
             renewedOrDeletedFieldsAbortTheWholeExpiryBatch},
            {"expiry revision conflict",
             expiryCleanupRejectsChangedRevisionBeforeMulti},
            {"invalid expiry candidates",
             expiryCleanupRejectsNonExpiringFutureAndIllegalEntries},
            {"expiry EXEC errors",
             expiryCleanupReportsExecErrorsWithoutPublishingRevision},
        }
    );
    if (contract_result != 0) {
        return contract_result;
    }
    return 0;
}
