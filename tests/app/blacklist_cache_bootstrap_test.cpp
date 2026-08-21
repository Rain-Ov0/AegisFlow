#include "aegisflow/app/blacklist_cache_bootstrap.hpp"
#include "aegisflow/risk/blacklist_mutation.hpp"
#include "aegisflow/storage/blacklist_redis_store.hpp"
#include "aegisflow/storage/mysql_dao.hpp"
#include "aegisflow/storage/redis_connection.hpp"

#include "tests/support/test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using aegisflow::storage::BlacklistRedisStore;
using aegisflow::storage::MysqlConfig;
using aegisflow::storage::MysqlDao;
using aegisflow::storage::RedisCommandStatus;
using aegisflow::storage::RedisConfig;
using aegisflow::storage::RedisConnection;
using aegisflow::storage::RedisDeadline;
using aegisflow::storage::RedisKeySet;
using aegisflow::storage::RedisValueKind;
using aegisflow::storage::StoreStatus;
using aegisflow::test::require;

RedisDeadline deadline() {
    return std::chrono::steady_clock::now() + std::chrono::seconds(5);
}

std::optional<RedisConfig> redisConfig() {
    const char* host = std::getenv("AEGISFLOW_TEST_REDIS_HOST");
    const char* port = std::getenv("AEGISFLOW_TEST_REDIS_PORT");
    if (host == nullptr || *host == '\0' || port == nullptr || *port == '\0') {
        return std::nullopt;
    }
    std::size_t parsed = 0;
    const auto port_value = std::stoul(port, &parsed);
    if (parsed != std::string_view(port).size() || port_value == 0 ||
        port_value > 65'535) {
        throw std::runtime_error("AEGISFLOW_TEST_REDIS_PORT invalid");
    }
    RedisConfig config;
    config.host = host;
    config.port = static_cast<std::uint16_t>(port_value);
    if (const char* value = std::getenv("AEGISFLOW_TEST_REDIS_USERNAME")) {
        config.username = value;
    }
    if (const char* value = std::getenv("AEGISFLOW_TEST_REDIS_PASSWORD")) {
        config.password = value;
    }
    if (const char* value = std::getenv("AEGISFLOW_TEST_REDIS_DATABASE")) {
        std::size_t db_parsed = 0;
        const auto database = std::stoul(value, &db_parsed);
        if (db_parsed != std::string_view(value).size() || database > 15) {
            throw std::runtime_error("AEGISFLOW_TEST_REDIS_DATABASE invalid");
        }
        config.database = static_cast<std::uint32_t>(database);
    }
    return config;
}

std::optional<MysqlConfig> mysqlConfig() {
    const char* permission =
        std::getenv("AEGISFLOW_TEST_MYSQL_ALLOW_MUTATION");
    const char* host = std::getenv("AEGISFLOW_TEST_MYSQL_HOST");
    const char* port = std::getenv("AEGISFLOW_TEST_MYSQL_PORT");
    const char* user = std::getenv("AEGISFLOW_TEST_MYSQL_USER");
    const char* database = std::getenv("AEGISFLOW_TEST_MYSQL_DATABASE");
    if (permission == nullptr ||
        std::string_view(permission) != "dedicated-test-database" ||
        host == nullptr || *host == '\0' || port == nullptr || *port == '\0' ||
        user == nullptr || *user == '\0' || database == nullptr ||
        *database == '\0') {
        return std::nullopt;
    }
    std::size_t parsed = 0;
    const auto port_value = std::stoul(port, &parsed);
    if (parsed != std::string_view(port).size() || port_value == 0 ||
        port_value > 65'535) {
        throw std::runtime_error("AEGISFLOW_TEST_MYSQL_PORT invalid");
    }
    MysqlConfig config;
    config.host = host;
    config.port = static_cast<std::uint16_t>(port_value);
    config.user = user;
    if (const char* value = std::getenv("AEGISFLOW_TEST_MYSQL_PASSWORD")) {
        config.password = value;
    }
    config.database = database;
    return config;
}

class ExactRedisCleanup final {
public:
    ExactRedisCleanup(RedisConnection& connection, RedisKeySet keys)
        : connection_(&connection), keys_(std::move(keys)) {}
    ~ExactRedisCleanup() {
        std::vector<std::string> command{"DEL"};
        const auto keys = keys_.all();
        command.insert(command.end(), keys.begin(), keys.end());
        (void)connection_->command(command, deadline());
    }

private:
    RedisConnection* connection_;
    RedisKeySet keys_;
};

RedisKeySet uniqueKeys(const std::string_view suffix) {
    return RedisKeySet::fromPrefix(
        "aegisflow:test:bootstrap:" +
        std::to_string(static_cast<long long>(::getpid())) + ":" +
        std::to_string(std::chrono::steady_clock::now()
                           .time_since_epoch().count()) +
        ":" + std::string(suffix));
}

void addPending(
    RedisConnection& connection,
    const RedisKeySet& keys,
    std::vector<std::string> fields
) {
    std::vector<std::string> command{"XADD", keys.pending, "*"};
    command.insert(command.end(), fields.begin(), fields.end());
    const auto result = connection.command(command, deadline());
    require(result.status == RedisCommandStatus::Ok &&
                result.value.kind == RedisValueKind::String &&
                !result.value.text.empty(),
            "cannot create isolated pending entry");
}

std::int64_t integerCommand(
    RedisConnection& connection,
    std::vector<std::string> argv
) {
    const auto result = connection.command(argv, deadline());
    require(result.status == RedisCommandStatus::Ok &&
                result.value.kind == RedisValueKind::Integer,
            "expected Redis integer reply");
    return result.value.integer;
}

void cleanupExactAndVerify(
    const RedisConfig& config,
    const RedisKeySet& keys
) {
    auto cleanup = RedisConnection::connect(config, deadline());
    require(cleanup != nullptr, "cannot create Redis cleanup connection");
    const auto all = keys.all();
    std::vector<std::string> remove{"DEL"};
    remove.insert(remove.end(), all.begin(), all.end());
    const auto result = cleanup->command(remove, deadline());
    require(result.status == RedisCommandStatus::Ok &&
                result.value.kind == RedisValueKind::Integer &&
                result.value.integer >= 0,
            "exact Redis cleanup failed");
    std::vector<std::string> exists{"EXISTS"};
    exists.insert(exists.end(), all.begin(), all.end());
    require(integerCommand(*cleanup, exists) == 0,
            "Redis bootstrap prefix was not fully removed");
}

void mysqlFailureKeepsPending(
    RedisConnection& connection,
    const RedisConfig& config
) {
    const auto keys = uniqueKeys("mysql-failure");
    ExactRedisCleanup cleanup(connection, keys);
    addPending(
        connection,
        keys,
        {"operation", "DISABLE", "type", "USER", "id", "keep-me"});

    MysqlConfig invalid;
    invalid.host = "127.0.0.1";
    invalid.port = 1;
    invalid.connect_timeout_sec = 1;
    MysqlDao mysql(invalid);
    BlacklistRedisStore store(connection, keys);
    const auto result = aegisflow::app::initializeBlacklistCache(
        store, mysql, 8, 8, deadline());
    require(result.status == StoreStatus::IoError,
            "MySQL failure must fail bootstrap");
    require(integerCommand(connection, {"XLEN", keys.pending}) == 1,
            "uncommitted pending entry must remain in Stream");
    require(integerCommand(connection, {"EXISTS", keys.cache_ready}) == 0,
            "failed bootstrap must not publish ready");
    cleanupExactAndVerify(config, keys);
}

void pendingIsCommittedBeforeColdRebuild(
    RedisConnection& connection,
    const RedisConfig& redis_config,
    const MysqlConfig& config
) {
    MysqlDao mysql(config);
    require(mysql.connect(deadline()),
            "cannot connect to dedicated MySQL test database");
    const std::vector<aegisflow::risk::BlacklistMutation> clear{
        aegisflow::risk::BlacklistMutation::clearAll()};
    require(mysql.applyBlacklistMutations(clear, deadline()),
            "initial MySQL clear failed");
    struct MysqlCleanup final {
        MysqlDao* mysql;
        aegisflow::base::ArrayView<
            const aegisflow::risk::BlacklistMutation> clear;
        ~MysqlCleanup() {
            (void)mysql->applyBlacklistMutations(clear, deadline());
        }
    } mysql_cleanup{&mysql, clear};

    const auto keys = uniqueKeys("joint");
    ExactRedisCleanup redis_cleanup(connection, keys);
    addPending(
        connection,
        keys,
        {"operation", "UPSERT", "type", "IP", "id", "2001:db8::71",
         "reason", "bootstrap", "expire_at_ms", "0"});
    addPending(
        connection,
        keys,
        {"operation", "BOGUS"});

    BlacklistRedisStore store(connection, keys);
    const auto result = aegisflow::app::initializeBlacklistCache(
        store, mysql, 8, 8, deadline());
    require(result.status == StoreStatus::Ok &&
                result.snapshot.status == StoreStatus::Ok &&
                result.snapshot.revision == 1 &&
                result.snapshot.entries.size() == 1 &&
                result.snapshot.entries.front().id == "2001:db8::71",
            "pending replay and cold snapshot are incomplete");
    require(integerCommand(connection, {"XLEN", keys.pending}) == 0,
            "committed and malformed entries must be XDEL'd");
    require(integerCommand(connection, {"HLEN", keys.ips}) == 1,
            "cold rebuild did not populate typed IP Hash");
    const auto counts = mysql.countEnabledBlacklists(deadline());
    require(mysql.lastBlacklistCountSucceeded() && counts.users == 0 &&
                counts.ips == 1 && counts.devices == 0,
            "pending replay did not commit exactly one MySQL row");
    cleanupExactAndVerify(redis_config, keys);
}

}  // namespace

int main() {
    try {
        const auto redis = redisConfig();
        if (!redis.has_value()) {
            std::cout << "[SKIP] blacklist_cache_bootstrap: set "
                         "AEGISFLOW_TEST_REDIS_HOST/PORT\n";
            return 0;
        }
        auto connection = RedisConnection::connect(*redis, deadline());
        require(connection != nullptr, "cannot connect to explicit Redis test");
        mysqlFailureKeepsPending(*connection, *redis);
        std::cout << "[PASS] MySQL failure keeps pending\n";

        const auto mysql = mysqlConfig();
        if (!mysql.has_value()) {
            std::cout << "[SKIP] Redis/MySQL cold bootstrap: set dedicated "
                         "MySQL env and mutation opt-in\n";
            return 0;
        }
        pendingIsCommittedBeforeColdRebuild(*connection, *redis, *mysql);
        std::cout << "[PASS] pending commit before cold rebuild\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] blacklist_cache_bootstrap: " << error.what()
                  << '\n';
        return 1;
    }
}
