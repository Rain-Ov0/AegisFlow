#include "aegisflow/domain/ip_address.hpp"
#include "aegisflow/risk/blacklist_mutation.hpp"
#include "aegisflow/storage/mysql_dao.hpp"

#include "tests/support/test_harness.hpp"

#include <mysql.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using aegisflow::risk::BlacklistMutation;
using aegisflow::risk::BlacklistMutationOperation;
using aegisflow::risk::EntityType;
using aegisflow::test::require;

aegisflow::storage::MysqlDeadline deadline() {
    return std::chrono::steady_clock::now() + std::chrono::seconds(10);
}

void ipTextAndBytesShareOneCanonicalValue() {
    const auto ipv4 = aegisflow::domain::IpAddress::parse("192.0.2.17");
    require(ipv4.has_value(), "IPv4 必须可解析");
    require(ipv4->canonicalText() == "192.0.2.17", "IPv4 规范文本错误");
    require(ipv4->isIpv4() && ipv4->bytes().size() == 4,
            "IPv4 必须保存 4 字节");
    const auto ipv4_roundtrip =
        aegisflow::domain::IpAddress::fromBytes(ipv4->bytes());
    require(ipv4_roundtrip == ipv4, "IPv4 文本/二进制往返必须一致");

    const auto ipv6 = aegisflow::domain::IpAddress::parse(
        "2001:0DB8:0000:0000:0000:0000:0000:0017");
    require(ipv6.has_value(), "IPv6 必须可解析");
    require(ipv6->canonicalText() == "2001:db8::17", "IPv6 必须压缩并小写");
    require(!ipv6->isIpv4() && ipv6->bytes().size() == 16,
            "IPv6 必须保存 16 字节");
    const auto ipv6_roundtrip =
        aegisflow::domain::IpAddress::fromBytes(ipv6->bytes());
    require(ipv6_roundtrip == ipv6, "IPv6 文本/二进制往返必须一致");

    const auto mapped =
        aegisflow::domain::IpAddress::parse("::ffff:192.0.2.17");
    require(mapped.has_value(), "IPv4-mapped IPv6 必须可解析");
    require(
        mapped->canonicalText() == "192.0.2.17" && mapped->isIpv4() &&
            mapped->bytes().size() == 4,
        "IPv4-mapped IPv6 必须收敛为普通 IPv4"
    );

    require(!aegisflow::domain::IpAddress::parse("bad-ip").has_value(),
            "非法 IP 不得产生值对象");
    const std::vector<std::uint8_t> invalid_bytes(5, 0);
    require(
        !aegisflow::domain::IpAddress::fromBytes(invalid_bytes).has_value(),
        "IP 二进制长度只能是 4 或 16"
    );
}

void mutationFactoriesProtectAllOperationInvariants() {
    auto user = BlacklistMutation::upsert(
        EntityType::User, "user-1", "manual", 0);
    require(user.has_value(), "永久 user UPSERT 必须可构造");
    require(
        user->operation() == BlacklistMutationOperation::Upsert &&
            user->entityType() == EntityType::User && user->id() == "user-1" &&
            user->reason() == "manual" && user->expireAtMs() == 0,
        "UPSERT 必须完整保存已校验字段"
    );

    auto ip = BlacklistMutation::upsert(
        EntityType::Ip,
        "2001:0db8:0:0:0:0:0:1",
        "manual",
        2'000'000'000'123ULL
    );
    require(ip.has_value() && ip->id() == "2001:db8::1",
            "IP mutation 必须保存规范文本");

    auto mapped = BlacklistMutation::disable(
        EntityType::Ip, "::ffff:192.0.2.1");
    require(
        mapped.has_value() && mapped->id() == "192.0.2.1" &&
            mapped->reason().empty() && mapped->expireAtMs() == 0,
        "DISABLE 必须规范化 IP 且不携带 UPSERT 字段"
    );

    const auto clear = BlacklistMutation::clearAll();
    require(
        clear.operation() == BlacklistMutationOperation::ClearAll &&
            !clear.entityType().has_value() && clear.id().empty() &&
            clear.reason().empty() && clear.expireAtMs() == 0,
        "CLEAR_ALL 不得携带实体或 UPSERT 字段"
    );

    require(
        !BlacklistMutation::upsert(EntityType::User, "", "reason", 0)
             .has_value(),
        "空实体 ID 必须被拒绝"
    );
    require(
        !BlacklistMutation::upsert(
             EntityType::Device,
             std::string(BlacklistMutation::kMaxEntityIdBytes + 1, 'd'),
             "reason",
             0).has_value(),
        "超长实体 ID 必须被拒绝"
    );
    require(
        !BlacklistMutation::upsert(EntityType::Ip, "bad-ip", "reason", 0)
             .has_value(),
        "非法 IP mutation 必须被拒绝"
    );
    require(
        !BlacklistMutation::upsert(EntityType::User, "user", "", 0)
             .has_value(),
        "UPSERT 的空 reason 必须被拒绝"
    );
    require(
        !BlacklistMutation::upsert(
             EntityType::User,
             "user",
             std::string(BlacklistMutation::kMaxReasonBytes + 1, 'r'),
             0).has_value(),
        "超长 reason 必须被拒绝"
    );
    require(
        BlacklistMutation::upsert(
            EntityType::Device,
            std::string(BlacklistMutation::kMaxEntityIdBytes, 'd'),
            std::string(BlacklistMutation::kMaxReasonBytes, 'r'),
            BlacklistMutation::kMaxExpireAtMs).has_value(),
        "ID、reason 和 DATETIME(3) 的精确上界必须可构造"
    );
    require(
        !BlacklistMutation::upsert(
             EntityType::User,
             "user",
             "reason",
             BlacklistMutation::kMaxExpireAtMs + 1).has_value(),
        "MySQL DATETIME(3) 范围外的过期时间必须被拒绝"
    );
    require(
        !BlacklistMutation::disable(
             static_cast<EntityType>(99), "entity").has_value(),
        "非法实体枚举必须被拒绝"
    );
}

void expiredDeadlineStopsBeforeMysqlIo() {
    aegisflow::storage::MysqlConfig config;
    config.host = "203.0.113.254";
    config.port = 65'535;
    config.connect_timeout_sec = 30;
    config.read_timeout_sec = 30;
    config.write_timeout_sec = 30;
    aegisflow::storage::MysqlDao dao(config);
    const auto expired = std::chrono::steady_clock::now();

    require(!dao.connect(expired),
            "已过期 deadline 必须在网络连接前失败");
    const std::vector<BlacklistMutation> clear{
        BlacklistMutation::clearAll(),
    };
    require(!dao.applyBlacklistMutations(clear, expired),
            "已过期 deadline 不得开启事务");
    require(dao.loadEnabledBlacklists(expired).empty() &&
                !dao.lastBlacklistLoadSucceeded(),
            "已过期 deadline 不得读取冷启快照");
    require(dao.countEnabledBlacklists(expired).empty() &&
                !dao.lastBlacklistCountSucceeded(),
            "已过期 deadline 不得读取 active count");

    config.host = "127.0.0.1";
    config.connect_timeout_sec = 0;
    aegisflow::storage::MysqlDao unlimited_timeout(config);
    require(!unlimited_timeout.connect(deadline()),
            "0 秒 mysqlclient timeout 会去掉上界，本契约必须拒绝");
}

std::optional<aegisflow::storage::MysqlConfig> integrationConfig() {
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
    unsigned long port_value = 0;
    try {
        port_value = std::stoul(port, &parsed);
    } catch (...) {
        throw std::runtime_error("AEGISFLOW_TEST_MYSQL_PORT 不是有效端口");
    }
    if (parsed != std::string_view(port).size() || port_value == 0 ||
        port_value > 65'535) {
        throw std::runtime_error("AEGISFLOW_TEST_MYSQL_PORT 超出端口范围");
    }

    const char* password = std::getenv("AEGISFLOW_TEST_MYSQL_PASSWORD");
    aegisflow::storage::MysqlConfig config;
    config.host = host;
    config.port = static_cast<std::uint16_t>(port_value);
    config.user = user;
    config.password = password == nullptr ? "" : password;
    config.database = database;
    return config;
}

bool containsEntry(
    const std::vector<aegisflow::risk::BlacklistEntry>& entries,
    const EntityType type,
    const std::string_view id,
    const std::uint64_t expiry
) {
    return std::any_of(
        entries.begin(), entries.end(), [&](const auto& entry) {
            return entry.type == type && entry.id == id &&
                   entry.expire_at_ms == expiry;
        });
}

BlacklistMutation requireMutation(
    std::optional<BlacklistMutation> mutation,
    const std::string_view message
) {
    require(mutation.has_value(), message);
    return std::move(*mutation);
}

void explainColdStartQueries(
    const aegisflow::storage::MysqlConfig& config
) {
    struct ConnectionDeleter {
        void operator()(MYSQL* connection) const noexcept {
            if (connection != nullptr) {
                mysql_close(connection);
            }
        }
    };
    struct ResultDeleter {
        void operator()(MYSQL_RES* result) const noexcept {
            if (result != nullptr) {
                mysql_free_result(result);
            }
        }
    };

    std::unique_ptr<MYSQL, ConnectionDeleter> connection(mysql_init(nullptr));
    require(connection != nullptr, "EXPLAIN mysql_init 失败");
    require(
        mysql_real_connect(
            connection.get(),
            config.host.c_str(),
            config.user.c_str(),
            config.password.c_str(),
            config.database.c_str(),
            config.port,
            nullptr,
            0) != nullptr,
        "EXPLAIN 连接 MySQL 失败"
    );

    struct ExplainCase {
        std::string_view table;
        std::string_view index;
        std::string_view sql;
    };
    static constexpr std::array<ExplainCase, 3> cases{{
        {
            "risk_blacklist_user",
            "idx_blacklist_user_active_expire",
            "EXPLAIN SELECT user_id, expire_at FROM risk_blacklist_user "
            "WHERE enabled = TRUE AND "
            "(expire_at IS NULL OR expire_at > UTC_TIMESTAMP(3))",
        },
        {
            "risk_blacklist_ip",
            "idx_blacklist_ip_active_expire",
            "EXPLAIN SELECT ip, expire_at FROM risk_blacklist_ip "
            "WHERE enabled = TRUE AND "
            "(expire_at IS NULL OR expire_at > UTC_TIMESTAMP(3))",
        },
        {
            "risk_blacklist_device",
            "idx_blacklist_device_active_expire",
            "EXPLAIN SELECT device_id, expire_at FROM risk_blacklist_device "
            "WHERE enabled = TRUE AND "
            "(expire_at IS NULL OR expire_at > UTC_TIMESTAMP(3))",
        },
    }};

    for (const auto& item : cases) {
        require(mysql_query(connection.get(), item.sql.data()) == 0,
                "冷启查询 EXPLAIN 失败");
        std::unique_ptr<MYSQL_RES, ResultDeleter> result(
            mysql_store_result(connection.get()));
        require(result != nullptr, "EXPLAIN 结果为空");

        MYSQL_FIELD* fields = mysql_fetch_fields(result.get());
        const unsigned int field_count = mysql_num_fields(result.get());
        std::optional<unsigned int> possible_keys_column;
        std::optional<unsigned int> key_column;
        std::optional<unsigned int> rows_column;
        for (unsigned int index = 0; index < field_count; ++index) {
            const std::string_view name(fields[index].name);
            if (name == "possible_keys") {
                possible_keys_column = index;
            } else if (name == "key") {
                key_column = index;
            } else if (name == "rows") {
                rows_column = index;
            }
        }
        require(
            possible_keys_column.has_value() && key_column.has_value() &&
                rows_column.has_value(),
            "EXPLAIN 缺少 possible_keys/key/rows 列"
        );

        MYSQL_ROW row = mysql_fetch_row(result.get());
        const unsigned long* lengths = mysql_fetch_lengths(result.get());
        require(row != nullptr && lengths != nullptr, "EXPLAIN 没有执行计划行");
        const auto fieldText = [&](const unsigned int index) {
            return row[index] == nullptr
                       ? std::string("NULL")
                       : std::string(row[index], lengths[index]);
        };
        std::cout << "[INFO] EXPLAIN table=" << item.table
                  << " expected_index=" << item.index
                  << " possible_keys=" << fieldText(*possible_keys_column)
                  << " key=" << fieldText(*key_column)
                  << " rows=" << fieldText(*rows_column) << '\n';
    }
}

void mysqlRoundTripAndOrderedTransaction(
    const aegisflow::storage::MysqlConfig& config
) {
    // EXPLAIN 只记录优化器的真实选择。小表选择全表扫描也是
    // 合法计划，测试不通过 FORCE INDEX 或硬断言伪造命中结果。
    explainColdStartQueries(config);

    aegisflow::storage::MysqlDao dao(config);
    require(dao.connect(deadline()), "无法使用显式测试凭据连接 MySQL");

    const std::vector<BlacklistMutation> clear{
        BlacklistMutation::clearAll(),
    };
    require(dao.applyBlacklistMutations(clear, deadline()),
            "测试库初始 CLEAR_ALL 失败");

    struct Cleanup final {
        aegisflow::storage::MysqlDao* dao = nullptr;
        const std::vector<BlacklistMutation>* clear = nullptr;
        ~Cleanup() {
            if (dao != nullptr && clear != nullptr) {
                (void)dao->applyBlacklistMutations(*clear, deadline());
            }
        }
    } cleanup{&dao, &clear};

    const std::vector<BlacklistMutation> deadline_baseline{
        requireMutation(
            BlacklistMutation::upsert(
                EntityType::User, "deadline-user", "deadline-test", 0),
            "deadline 基线 mutation 构造失败"),
    };
    require(dao.applyBlacklistMutations(deadline_baseline, deadline()),
            "deadline 基线写入失败");
    require(dao.countEnabledBlacklists(deadline()) ==
                aegisflow::storage::BlacklistCounts{1, 0, 0} &&
                dao.lastBlacklistCountSucceeded(),
            "deadline 无副作用测试的基线状态错误");

    const auto expired = std::chrono::steady_clock::now();
    require(!dao.applyBlacklistMutations(clear, expired),
            "已连接 DAO 不得用过期 deadline 开启 CLEAR_ALL");
    require(dao.loadEnabledBlacklists(expired).empty() &&
                !dao.lastBlacklistLoadSucceeded(),
            "已连接 DAO 的过期 load 必须在 SQL 前失败");
    require(dao.countEnabledBlacklists(expired).empty() &&
                !dao.lastBlacklistCountSucceeded(),
            "已连接 DAO 的过期 count 必须在 SQL 前失败");
    require(dao.countEnabledBlacklists(deadline()) ==
                aegisflow::storage::BlacklistCounts{1, 0, 0} &&
                dao.lastBlacklistCountSucceeded(),
            "过期 CLEAR_ALL 不得改变已知 MySQL 状态");
    require(dao.applyBlacklistMutations(clear, deadline()),
            "deadline 基线收尾 CLEAR_ALL 失败");

    const auto now_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    const std::uint64_t expire_at_ms =
        ((now_ms / 1'000) + 3'600) * 1'000 + 123;

    std::vector<BlacklistMutation> initial;
    initial.push_back(requireMutation(
        BlacklistMutation::upsert(
            EntityType::User, "p3-user", "p3-user", expire_at_ms),
        "user mutation 构造失败"));
    initial.push_back(requireMutation(
        BlacklistMutation::upsert(
            EntityType::Ip, "192.0.2.31", "p3-ipv4", expire_at_ms),
        "IPv4 mutation 构造失败"));
    initial.push_back(requireMutation(
        BlacklistMutation::upsert(
            EntityType::Ip,
            "2001:0db8:0:0:0:0:0:31",
            "p3-ipv6",
            expire_at_ms),
        "IPv6 mutation 构造失败"));
    initial.push_back(requireMutation(
        BlacklistMutation::upsert(
            EntityType::Device, "p3-device", "p3-device", 0),
        "device mutation 构造失败"));

    require(dao.applyBlacklistMutations(initial, deadline()),
            "三类 UPSERT 事务失败");
    auto counts = dao.countEnabledBlacklists(deadline());
    require(dao.lastBlacklistCountSucceeded(), "三表 enabled count 失败");
    require(counts == aegisflow::storage::BlacklistCounts{1, 2, 1},
            "UPSERT 后三表计数错误");

    auto entries = dao.loadEnabledBlacklists(deadline());
    require(dao.lastBlacklistLoadSucceeded(), "三表冷启读取失败");
    require(entries.size() == 4, "三表冷启读取数量错误");
    require(
        containsEntry(entries, EntityType::User, "p3-user", expire_at_ms) &&
            containsEntry(
                entries, EntityType::Ip, "192.0.2.31", expire_at_ms) &&
            containsEntry(
                entries, EntityType::Ip, "2001:db8::31", expire_at_ms) &&
            containsEntry(entries, EntityType::Device, "p3-device", 0),
        "冷启读取必须保留 canonical IP 与 UTC 毫秒过期时间"
    );

    require(dao.applyBlacklistMutations(initial, deadline()),
            "幂等 UPSERT 重放失败");
    counts = dao.countEnabledBlacklists(deadline());
    require(dao.lastBlacklistCountSucceeded() &&
                counts == aegisflow::storage::BlacklistCounts{1, 2, 1},
            "UPSERT 重放不得产生重复行");

    std::vector<BlacklistMutation> disables;
    disables.push_back(requireMutation(
        BlacklistMutation::disable(EntityType::Ip, "2001:db8::31"),
        "IPv6 disable 构造失败"));
    disables.push_back(requireMutation(
        BlacklistMutation::disable(EntityType::Device, "p3-device"),
        "device disable 构造失败"));
    require(dao.applyBlacklistMutations(disables, deadline()),
            "IP/device disable 事务失败");
    require(dao.applyBlacklistMutations(disables, deadline()),
            "IP/device disable 重放失败");
    counts = dao.countEnabledBlacklists(deadline());
    require(
        dao.lastBlacklistCountSucceeded() &&
            counts == aegisflow::storage::BlacklistCounts{1, 1, 0},
        "IP/device disable 必须幂等且只影响目标实体"
    );
    require(dao.applyBlacklistMutations(initial, deadline()),
            "disable 后重新启用失败");

    std::vector<BlacklistMutation> ordered;
    ordered.push_back(requireMutation(
        BlacklistMutation::upsert(
            EntityType::User, "ordered-user", "first", expire_at_ms),
        "ordered upsert 构造失败"));
    ordered.push_back(requireMutation(
        BlacklistMutation::disable(EntityType::User, "ordered-user"),
        "ordered disable 构造失败"));
    ordered.push_back(requireMutation(
        BlacklistMutation::upsert(
            EntityType::User, "ordered-user", "last", expire_at_ms),
        "ordered re-upsert 构造失败"));
    require(dao.applyBlacklistMutations(ordered, deadline()),
            "顺序 upsert/disable/upsert 失败");
    entries = dao.loadEnabledBlacklists(deadline());
    require(
        dao.lastBlacklistLoadSucceeded() &&
            containsEntry(
                entries, EntityType::User, "ordered-user", expire_at_ms),
        "事务必须按 mutation 输入顺序执行"
    );

    std::vector<BlacklistMutation> clear_then_upsert;
    clear_then_upsert.push_back(requireMutation(
        BlacklistMutation::upsert(
            EntityType::Device, "before-clear", "before", expire_at_ms),
        "clear 前 mutation 构造失败"));
    clear_then_upsert.push_back(BlacklistMutation::clearAll());
    clear_then_upsert.push_back(requireMutation(
        BlacklistMutation::upsert(
            EntityType::Ip, "198.51.100.31", "after", expire_at_ms),
        "clear 后 mutation 构造失败"));
    require(dao.applyBlacklistMutations(clear_then_upsert, deadline()),
            "UPSERT/CLEAR_ALL/UPSERT 事务失败");
    counts = dao.countEnabledBlacklists(deadline());
    require(
        dao.lastBlacklistCountSucceeded() &&
            counts == aegisflow::storage::BlacklistCounts{0, 1, 0},
        "CLEAR_ALL 必须使前置变更失效且允许后续 UPSERT"
    );

    require(dao.applyBlacklistMutations(clear, deadline()),
            "测试库最终 CLEAR_ALL 失败");
    counts = dao.countEnabledBlacklists(deadline());
    require(dao.lastBlacklistCountSucceeded() && counts.empty(),
            "最终三表 enabled count 必须为零");
    cleanup.dao = nullptr;
}

}  // namespace

int main() {
    const int contract_status = aegisflow::test::runModule(
        "mysql_blacklist_contract",
        {
            {"IP 规范文本与二进制", ipTextAndBytesShareOneCanonicalValue},
            {"mutation 合法状态", mutationFactoriesProtectAllOperationInvariants},
            {"MySQL 绝对 deadline 前置拒绝", expiredDeadlineStopsBeforeMysqlIo},
        }
    );
    if (contract_status != 0) {
        return contract_status;
    }

    const auto config = integrationConfig();
    if (!config.has_value()) {
        std::cout
            << "[SKIP] MySQL 集成：需显式设置 "
               "AEGISFLOW_TEST_MYSQL_HOST/PORT/USER/DATABASE，并将 "
               "AEGISFLOW_TEST_MYSQL_ALLOW_MUTATION="
               "dedicated-test-database\n";
        return 0;
    }

    return aegisflow::test::runModule(
        "mysql_blacklist_integration",
        {{"UTC 往返与顺序事务", [config] {
              mysqlRoundTripAndOrderedTransaction(*config);
          }}}
    );
}
