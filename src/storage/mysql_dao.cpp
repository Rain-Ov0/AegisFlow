#include "aegisflow/storage/mysql_dao.hpp"

#include "aegisflow/domain/ip_address.hpp"

#include <mysql.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegisflow::storage {
namespace {

struct MysqlStatementDeleter {
    void operator()(MYSQL_STMT* statement) const noexcept {
        if (statement != nullptr) {
            mysql_stmt_close(statement);
        }
    }
};

using MysqlStatementPtr = std::unique_ptr<MYSQL_STMT, MysqlStatementDeleter>;

bool deadlineReached(const MysqlDeadline deadline) noexcept {
    return std::chrono::steady_clock::now() >= deadline;
}

std::optional<unsigned int> timeoutWithinDeadline(
    const unsigned int configured_seconds,
    const MysqlDeadline deadline
) noexcept {
    using namespace std::chrono;

    const auto now = steady_clock::now();
    if (configured_seconds == 0 || now >= deadline) {
        return std::nullopt;
    }

    const auto remaining = deadline - now;
    auto rounded_seconds = duration_cast<seconds>(remaining);
    if (duration_cast<steady_clock::duration>(rounded_seconds) < remaining) {
        ++rounded_seconds;
    }
    const auto seconds_count = std::max<std::int64_t>(
        1, rounded_seconds.count());
    return std::min<unsigned int>(
        configured_seconds,
        seconds_count > std::numeric_limits<unsigned int>::max()
            ? std::numeric_limits<unsigned int>::max()
            : static_cast<unsigned int>(seconds_count));
}

class MysqlTransaction final {
public:
    explicit MysqlTransaction(MYSQL* connection) noexcept
        : connection_(connection) {}

    MysqlTransaction(const MysqlTransaction&) = delete;
    MysqlTransaction& operator=(const MysqlTransaction&) = delete;

    ~MysqlTransaction() {
        if (active_) {
            mysql_rollback(connection_);
        }
    }

    [[nodiscard]] bool commit(const MysqlDeadline deadline) noexcept {
        if (!active_ || deadlineReached(deadline)) {
            return false;
        }
        const bool succeeded = mysql_commit(connection_) == 0;
        if (!succeeded) {
            return false;
        }
        // COMMIT 已明确成功时不能再回滚。若调用结束时才超时，
        // 上层收到 false 并可依赖幂等性重放，但不会误伤已提交数据。
        active_ = false;
        return !deadlineReached(deadline);
    }

private:
    MYSQL* connection_ = nullptr;
    bool active_ = true;
};

std::optional<MysqlStatementPtr> prepareStatement(
    MYSQL* connection,
    const std::string_view sql,
    const MysqlDeadline deadline
) {
    if (deadlineReached(deadline)) {
        return std::nullopt;
    }
    MysqlStatementPtr statement(mysql_stmt_init(connection));
    if (statement == nullptr ||
        mysql_stmt_prepare(statement.get(), sql.data(), sql.size()) != 0 ||
        deadlineReached(deadline)) {
        return std::nullopt;
    }
    return statement;
}

std::optional<MYSQL_TIME> toMysqlTime(const std::uint64_t epoch_ms) noexcept {
    if (epoch_ms == 0 ||
        epoch_ms > risk::BlacklistMutation::kMaxExpireAtMs ||
        epoch_ms / 1'000 > static_cast<std::uint64_t>(
                               std::numeric_limits<std::time_t>::max())) {
        return std::nullopt;
    }

    const auto seconds = static_cast<std::time_t>(epoch_ms / 1'000);
    std::tm utc{};
    if (::gmtime_r(&seconds, &utc) == nullptr) {
        return std::nullopt;
    }

    const int year_value = utc.tm_year + 1900;
    if (year_value < 1970 || year_value > 9999) {
        return std::nullopt;
    }

    MYSQL_TIME result{};
    result.year = static_cast<unsigned int>(year_value);
    result.month = static_cast<unsigned int>(utc.tm_mon + 1);
    result.day = static_cast<unsigned int>(utc.tm_mday);
    result.hour = static_cast<unsigned int>(utc.tm_hour);
    result.minute = static_cast<unsigned int>(utc.tm_min);
    result.second = static_cast<unsigned int>(utc.tm_sec);
    result.second_part = static_cast<unsigned long>(epoch_ms % 1'000) * 1'000;
    result.neg = false;
    result.time_type = MYSQL_TIMESTAMP_DATETIME;
    return result;
}

std::optional<std::uint64_t> fromMysqlTime(
    const MYSQL_TIME& value
) noexcept {
    if (value.neg || value.year < 1970 || value.year > 9999 ||
        value.month == 0 || value.month > 12 ||
        value.day == 0 || value.day > 31 ||
        value.hour > 23 || value.minute > 59 || value.second > 59 ||
        value.second_part > 999'999 || value.second_part % 1'000 != 0) {
        return std::nullopt;
    }

    std::tm utc{};
    utc.tm_year = static_cast<int>(value.year) - 1900;
    utc.tm_mon = static_cast<int>(value.month) - 1;
    utc.tm_mday = static_cast<int>(value.day);
    utc.tm_hour = static_cast<int>(value.hour);
    utc.tm_min = static_cast<int>(value.minute);
    utc.tm_sec = static_cast<int>(value.second);
    utc.tm_isdst = 0;
    errno = 0;
    const std::time_t seconds = ::timegm(&utc);
    if (seconds < 0 || errno == EOVERFLOW ||
        utc.tm_year != static_cast<int>(value.year) - 1900 ||
        utc.tm_mon != static_cast<int>(value.month) - 1 ||
        utc.tm_mday != static_cast<int>(value.day) ||
        utc.tm_hour != static_cast<int>(value.hour) ||
        utc.tm_min != static_cast<int>(value.minute) ||
        utc.tm_sec != static_cast<int>(value.second)) {
        return std::nullopt;
    }

    const auto whole_seconds = static_cast<std::uint64_t>(seconds);
    const auto milliseconds =
        static_cast<std::uint64_t>(value.second_part / 1'000);
    if (whole_seconds >
        (risk::BlacklistMutation::kMaxExpireAtMs - milliseconds) / 1'000) {
        return std::nullopt;
    }
    return whole_seconds * 1'000 + milliseconds;
}

std::optional<std::uint64_t> parseUInt64(
    const char* text,
    const unsigned long length
) {
    if (text == nullptr || length == 0) {
        return std::nullopt;
    }
    try {
        const std::string value(text, length);
        std::size_t parsed = 0;
        const std::uint64_t result = std::stoull(value, &parsed);
        if (parsed != value.size()) {
            return std::nullopt;
        }
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

bool appendTextEntries(
    MYSQL* connection,
    const risk::EntityType type,
    const std::string_view sql,
    std::vector<risk::BlacklistEntry>& entries,
    const MysqlDeadline deadline
) {
    auto prepared = prepareStatement(connection, sql, deadline);
    if (!prepared.has_value() || deadlineReached(deadline) ||
        mysql_stmt_execute(prepared->get()) != 0 || deadlineReached(deadline)) {
        return false;
    }

    std::array<char, risk::BlacklistMutation::kMaxEntityIdBytes> id{};
    unsigned long id_length = 0;
    bool id_is_null = false;
    bool id_error = false;
    MYSQL_TIME expiry{};
    bool expiry_is_null = false;
    bool expiry_error = false;

    std::array<MYSQL_BIND, 2> result{};
    result[0].buffer_type = MYSQL_TYPE_STRING;
    result[0].buffer = id.data();
    result[0].buffer_length = id.size();
    result[0].length = &id_length;
    result[0].is_null = &id_is_null;
    result[0].error = &id_error;
    result[1].buffer_type = MYSQL_TYPE_DATETIME;
    result[1].buffer = &expiry;
    result[1].is_null = &expiry_is_null;
    result[1].error = &expiry_error;

    if (deadlineReached(deadline) ||
        mysql_stmt_bind_result(prepared->get(), result.data()) != 0 ||
        deadlineReached(deadline) ||
        mysql_stmt_store_result(prepared->get()) != 0 ||
        deadlineReached(deadline)) {
        return false;
    }

    while (true) {
        id_length = 0;
        id_is_null = false;
        id_error = false;
        expiry = {};
        expiry_is_null = false;
        expiry_error = false;

        if (deadlineReached(deadline)) {
            return false;
        }
        const int status = mysql_stmt_fetch(prepared->get());
        if (deadlineReached(deadline)) {
            return false;
        }
        if (status == MYSQL_NO_DATA) {
            return true;
        }
        if (status != 0 || id_is_null || id_error || expiry_error ||
            id_length == 0 || id_length > id.size()) {
            return false;
        }

        const std::string entity_id(id.data(), id_length);
        if (entity_id.find('\0') != std::string::npos) {
            return false;
        }

        std::uint64_t expire_at_ms = 0;
        if (!expiry_is_null) {
            const auto converted = fromMysqlTime(expiry);
            if (!converted.has_value()) {
                return false;
            }
            expire_at_ms = *converted;
        }
        risk::BlacklistEntry entry;
        entry.type = type;
        entry.id = entity_id;
        entry.expire_at_ms = expire_at_ms;
        entries.push_back(std::move(entry));
    }
}

bool appendIpEntries(
    MYSQL* connection,
    const std::string_view sql,
    std::vector<risk::BlacklistEntry>& entries,
    const MysqlDeadline deadline
) {
    auto prepared = prepareStatement(connection, sql, deadline);
    if (!prepared.has_value() || deadlineReached(deadline) ||
        mysql_stmt_execute(prepared->get()) != 0 || deadlineReached(deadline)) {
        return false;
    }

    std::array<std::uint8_t, 16> ip{};
    unsigned long ip_length = 0;
    bool ip_is_null = false;
    bool ip_error = false;
    MYSQL_TIME expiry{};
    bool expiry_is_null = false;
    bool expiry_error = false;

    std::array<MYSQL_BIND, 2> result{};
    result[0].buffer_type = MYSQL_TYPE_BLOB;
    result[0].buffer = ip.data();
    result[0].buffer_length = ip.size();
    result[0].length = &ip_length;
    result[0].is_null = &ip_is_null;
    result[0].error = &ip_error;
    result[1].buffer_type = MYSQL_TYPE_DATETIME;
    result[1].buffer = &expiry;
    result[1].is_null = &expiry_is_null;
    result[1].error = &expiry_error;

    if (deadlineReached(deadline) ||
        mysql_stmt_bind_result(prepared->get(), result.data()) != 0 ||
        deadlineReached(deadline) ||
        mysql_stmt_store_result(prepared->get()) != 0 ||
        deadlineReached(deadline)) {
        return false;
    }

    while (true) {
        ip_length = 0;
        ip_is_null = false;
        ip_error = false;
        expiry = {};
        expiry_is_null = false;
        expiry_error = false;

        if (deadlineReached(deadline)) {
            return false;
        }
        const int status = mysql_stmt_fetch(prepared->get());
        if (deadlineReached(deadline)) {
            return false;
        }
        if (status == MYSQL_NO_DATA) {
            return true;
        }
        if (status != 0 || ip_is_null || ip_error || expiry_error ||
            (ip_length != 4 && ip_length != 16)) {
            return false;
        }

        const auto address = domain::IpAddress::fromBytes(
            {ip.data(), static_cast<std::size_t>(ip_length)});
        if (!address.has_value()) {
            return false;
        }

        std::uint64_t expire_at_ms = 0;
        if (!expiry_is_null) {
            const auto converted = fromMysqlTime(expiry);
            if (!converted.has_value()) {
                return false;
            }
            expire_at_ms = *converted;
        }
        risk::BlacklistEntry entry;
        entry.type = risk::EntityType::Ip;
        entry.id = address->canonicalText();
        entry.expire_at_ms = expire_at_ms;
        entries.push_back(std::move(entry));
    }
}

enum class StatementKind : std::size_t {
    UserUpsert,
    IpUpsert,
    DeviceUpsert,
    UserDisable,
    IpDisable,
    DeviceDisable,
    UserClear,
    IpClear,
    DeviceClear,
    Count,
};

constexpr std::array<std::string_view,
                     static_cast<std::size_t>(StatementKind::Count)>
    kMutationSql = {
        R"SQL(INSERT INTO risk_blacklist_user
    (user_id, reason, enabled, expire_at)
VALUES (?, ?, TRUE, ?)
ON DUPLICATE KEY UPDATE
    reason = ?, enabled = TRUE, expire_at = ?)SQL",
        R"SQL(INSERT INTO risk_blacklist_ip
    (ip, reason, enabled, expire_at)
VALUES (?, ?, TRUE, ?)
ON DUPLICATE KEY UPDATE
    reason = ?, enabled = TRUE, expire_at = ?)SQL",
        R"SQL(INSERT INTO risk_blacklist_device
    (device_id, reason, enabled, expire_at)
VALUES (?, ?, TRUE, ?)
ON DUPLICATE KEY UPDATE
    reason = ?, enabled = TRUE, expire_at = ?)SQL",
        "UPDATE risk_blacklist_user SET enabled = FALSE WHERE user_id = ?",
        "UPDATE risk_blacklist_ip SET enabled = FALSE WHERE ip = ?",
        "UPDATE risk_blacklist_device SET enabled = FALSE WHERE device_id = ?",
        "UPDATE risk_blacklist_user SET enabled = FALSE WHERE enabled = TRUE",
        "UPDATE risk_blacklist_ip SET enabled = FALSE WHERE enabled = TRUE",
        "UPDATE risk_blacklist_device SET enabled = FALSE WHERE enabled = TRUE",
    };

using StatementArray = std::array<
    MysqlStatementPtr,
    static_cast<std::size_t>(StatementKind::Count)
>;

MYSQL_STMT* statementFor(
    MYSQL* connection,
    StatementArray& statements,
    const StatementKind kind,
    const MysqlDeadline deadline
) {
    if (deadlineReached(deadline)) {
        return nullptr;
    }
    const auto index = static_cast<std::size_t>(kind);
    if (statements[index] == nullptr) {
        auto statement = prepareStatement(
            connection, kMutationSql[index], deadline);
        if (!statement.has_value()) {
            return nullptr;
        }
        statements[index] = std::move(*statement);
    }
    if (deadlineReached(deadline) ||
        mysql_stmt_reset(statements[index].get()) != 0 ||
        deadlineReached(deadline)) {
        return nullptr;
    }
    return statements[index].get();
}

std::optional<StatementKind> upsertKind(const risk::EntityType type) noexcept {
    switch (type) {
    case risk::EntityType::User:
        return StatementKind::UserUpsert;
    case risk::EntityType::Ip:
        return StatementKind::IpUpsert;
    case risk::EntityType::Device:
        return StatementKind::DeviceUpsert;
    }
    return std::nullopt;
}

std::optional<StatementKind> disableKind(const risk::EntityType type) noexcept {
    switch (type) {
    case risk::EntityType::User:
        return StatementKind::UserDisable;
    case risk::EntityType::Ip:
        return StatementKind::IpDisable;
    case risk::EntityType::Device:
        return StatementKind::DeviceDisable;
    }
    return std::nullopt;
}

bool executeUpsert(
    MYSQL* connection,
    StatementArray& statements,
    const risk::BlacklistMutation& mutation,
    const MysqlDeadline deadline
) {
    if (deadlineReached(deadline) || !mutation.entityType().has_value()) {
        return false;
    }
    const auto kind = upsertKind(*mutation.entityType());
    if (!kind.has_value()) {
        return false;
    }
    MYSQL_STMT* statement = statementFor(
        connection, statements, *kind, deadline);
    if (statement == nullptr) {
        return false;
    }

    std::optional<domain::IpAddress> ip;
    if (*mutation.entityType() == risk::EntityType::Ip) {
        ip = domain::IpAddress::parse(mutation.id());
        if (!ip.has_value()) {
            return false;
        }
    }

    unsigned long id_length = static_cast<unsigned long>(mutation.id().size());
    unsigned long reason_length =
        static_cast<unsigned long>(mutation.reason().size());
    bool expiry_is_null = mutation.expireAtMs() == 0;
    MYSQL_TIME expiry{};
    if (!expiry_is_null) {
        const auto converted = toMysqlTime(mutation.expireAtMs());
        if (!converted.has_value()) {
            return false;
        }
        expiry = *converted;
    }

    std::array<MYSQL_BIND, 5> parameters{};
    const bool is_ip = *mutation.entityType() == risk::EntityType::Ip;
    parameters[0].buffer_type = is_ip ? MYSQL_TYPE_BLOB : MYSQL_TYPE_STRING;
    parameters[0].buffer = is_ip
                               ? static_cast<void*>(
                                     const_cast<std::uint8_t*>(ip->bytes().data()))
                               : static_cast<void*>(
                                     const_cast<char*>(mutation.id().data()));
    parameters[0].buffer_length = is_ip ? ip->bytes().size()
                                        : mutation.id().size();
    if (is_ip) {
        id_length = static_cast<unsigned long>(ip->bytes().size());
    }
    parameters[0].length = &id_length;

    for (const std::size_t index : {std::size_t{1}, std::size_t{3}}) {
        parameters[index].buffer_type = MYSQL_TYPE_STRING;
        parameters[index].buffer =
            const_cast<char*>(mutation.reason().data());
        parameters[index].buffer_length = mutation.reason().size();
        parameters[index].length = &reason_length;
    }
    for (const std::size_t index : {std::size_t{2}, std::size_t{4}}) {
        parameters[index].buffer_type = MYSQL_TYPE_DATETIME;
        parameters[index].buffer = &expiry;
        parameters[index].is_null = &expiry_is_null;
    }

    return !deadlineReached(deadline) &&
           mysql_stmt_bind_param(statement, parameters.data()) == 0 &&
           !deadlineReached(deadline) &&
           mysql_stmt_execute(statement) == 0 &&
           !deadlineReached(deadline);
}

bool executeDisable(
    MYSQL* connection,
    StatementArray& statements,
    const risk::BlacklistMutation& mutation,
    const MysqlDeadline deadline
) {
    if (deadlineReached(deadline) || !mutation.entityType().has_value()) {
        return false;
    }
    const auto kind = disableKind(*mutation.entityType());
    if (!kind.has_value()) {
        return false;
    }
    MYSQL_STMT* statement = statementFor(
        connection, statements, *kind, deadline);
    if (statement == nullptr) {
        return false;
    }

    std::optional<domain::IpAddress> ip;
    if (*mutation.entityType() == risk::EntityType::Ip) {
        ip = domain::IpAddress::parse(mutation.id());
        if (!ip.has_value()) {
            return false;
        }
    }

    const bool is_ip = *mutation.entityType() == risk::EntityType::Ip;
    unsigned long id_length = is_ip
                                  ? static_cast<unsigned long>(ip->bytes().size())
                                  : static_cast<unsigned long>(mutation.id().size());
    MYSQL_BIND parameter{};
    parameter.buffer_type = is_ip ? MYSQL_TYPE_BLOB : MYSQL_TYPE_STRING;
    parameter.buffer = is_ip
                           ? static_cast<void*>(
                                 const_cast<std::uint8_t*>(ip->bytes().data()))
                           : static_cast<void*>(
                                 const_cast<char*>(mutation.id().data()));
    parameter.buffer_length = id_length;
    parameter.length = &id_length;

    return !deadlineReached(deadline) &&
           mysql_stmt_bind_param(statement, &parameter) == 0 &&
           !deadlineReached(deadline) &&
           mysql_stmt_execute(statement) == 0 &&
           !deadlineReached(deadline);
}

bool executeClear(
    MYSQL* connection,
    StatementArray& statements,
    const MysqlDeadline deadline
) {
    for (const StatementKind kind : {
             StatementKind::UserClear,
             StatementKind::IpClear,
             StatementKind::DeviceClear,
         }) {
        MYSQL_STMT* statement = statementFor(
            connection, statements, kind, deadline);
        if (statement == nullptr || deadlineReached(deadline) ||
            mysql_stmt_execute(statement) != 0 || deadlineReached(deadline)) {
            return false;
        }
    }
    return true;
}

}  // 命名空间

class MysqlDao::Impl final {
public:
    explicit Impl(MysqlConfig mysql_config)
        : config(std::move(mysql_config)) {}

    ~Impl() { close(); }

    void close() noexcept {
        if (connection != nullptr) {
            mysql_close(connection);
            connection = nullptr;
        }
        last_blacklist_load_succeeded = false;
        last_blacklist_count_succeeded = false;
    }

    MysqlConfig config;
    MYSQL* connection = nullptr;
    bool last_blacklist_load_succeeded = false;
    bool last_blacklist_count_succeeded = false;
};

MysqlDao::MysqlDao(MysqlConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

MysqlDao::~MysqlDao() = default;

bool MysqlDao::connect(const MysqlDeadline deadline) {
    impl_->close();

    const auto connect_timeout = timeoutWithinDeadline(
        impl_->config.connect_timeout_sec, deadline);
    const auto read_timeout = timeoutWithinDeadline(
        impl_->config.read_timeout_sec, deadline);
    const auto write_timeout = timeoutWithinDeadline(
        impl_->config.write_timeout_sec, deadline);
    if (!connect_timeout.has_value() || !read_timeout.has_value() ||
        !write_timeout.has_value()) {
        return false;
    }

    MYSQL* handle = mysql_init(nullptr);
    if (handle == nullptr) {
        return false;
    }

    if (mysql_options(handle, MYSQL_OPT_CONNECT_TIMEOUT,
                      &*connect_timeout) != 0 ||
        mysql_options(handle, MYSQL_OPT_READ_TIMEOUT, &*read_timeout) != 0 ||
        mysql_options(handle, MYSQL_OPT_WRITE_TIMEOUT, &*write_timeout) != 0 ||
        deadlineReached(deadline)) {
        mysql_close(handle);
        return false;
    }

    MYSQL* connected = mysql_real_connect(
        handle,
        impl_->config.host.c_str(),
        impl_->config.user.c_str(),
        impl_->config.password.c_str(),
        impl_->config.database.c_str(),
        impl_->config.port,
        nullptr,
        0
    );
    if (connected == nullptr) {
        mysql_close(handle);
        return false;
    }
    impl_->connection = handle;
    if (deadlineReached(deadline)) {
        impl_->close();
        return false;
    }

    // DATETIME 本身不带时区。会话固定 UTC，使 NOW、默认时间戳
    // 和预处理绑定的 UTC 日历分量具有同一语义。
    if (deadlineReached(deadline) ||
        (!impl_->config.charset.empty() &&
         mysql_set_character_set(
             impl_->connection, impl_->config.charset.c_str()) != 0) ||
        deadlineReached(deadline) ||
        mysql_query(
            impl_->connection, "SET SESSION time_zone = '+00:00'") != 0 ||
        deadlineReached(deadline)) {
        impl_->close();
        return false;
    }
    return true;
}

bool MysqlDao::lastBlacklistLoadSucceeded() const noexcept {
    return impl_->last_blacklist_load_succeeded;
}

bool MysqlDao::lastBlacklistCountSucceeded() const noexcept {
    return impl_->last_blacklist_count_succeeded;
}

std::vector<risk::BlacklistEntry> MysqlDao::loadEnabledBlacklists(
    const MysqlDeadline deadline
) {
    std::vector<risk::BlacklistEntry> entries;
    impl_->last_blacklist_load_succeeded = false;
    if (impl_->connection == nullptr || deadlineReached(deadline)) {
        return entries;
    }

    static constexpr std::string_view kLoadUsers = R"SQL(
SELECT user_id, expire_at
FROM risk_blacklist_user
WHERE enabled = TRUE
  AND (expire_at IS NULL OR expire_at > UTC_TIMESTAMP(3))
)SQL";
    static constexpr std::string_view kLoadIps = R"SQL(
SELECT ip, expire_at
FROM risk_blacklist_ip
WHERE enabled = TRUE
  AND (expire_at IS NULL OR expire_at > UTC_TIMESTAMP(3))
)SQL";
    static constexpr std::string_view kLoadDevices = R"SQL(
SELECT device_id, expire_at
FROM risk_blacklist_device
WHERE enabled = TRUE
  AND (expire_at IS NULL OR expire_at > UTC_TIMESTAMP(3))
)SQL";

    if (!appendTextEntries(
            impl_->connection,
            risk::EntityType::User,
            kLoadUsers,
            entries,
            deadline) ||
        !appendIpEntries(impl_->connection, kLoadIps, entries, deadline) ||
        !appendTextEntries(
            impl_->connection,
            risk::EntityType::Device,
            kLoadDevices,
            entries,
            deadline) ||
        deadlineReached(deadline)) {
        entries.clear();
        return entries;
    }

    impl_->last_blacklist_load_succeeded = true;
    return entries;
}

bool MysqlDao::applyBlacklistMutations(
    const base::ArrayView<const risk::BlacklistMutation> mutations,
    const MysqlDeadline deadline
) {
    if (impl_->connection == nullptr || deadlineReached(deadline)) {
        return false;
    }
    if (mutations.empty()) {
        return true;
    }
    if (mysql_query(impl_->connection, "START TRANSACTION") != 0) {
        return false;
    }

    try {
        // 守卫在任何 false 返回或 C++ 异常展开时回滚，避免同一连接
        // 带着未完成事务进入下一个 maintenance 批次。
        MysqlTransaction transaction(impl_->connection);
        if (deadlineReached(deadline)) {
            return false;
        }
        StatementArray statements{};
        bool succeeded = true;
        // 顺序就是 Redis Stream 的业务顺序；同一事务内不做按类型重排，
        // 保证 UPSERT -> CLEAR_ALL -> UPSERT 的最终状态可预期。
        for (const auto& mutation : mutations) {
            switch (mutation.operation()) {
            case risk::BlacklistMutationOperation::Upsert:
                succeeded = executeUpsert(
                    impl_->connection, statements, mutation, deadline);
                break;
            case risk::BlacklistMutationOperation::Disable:
                succeeded = executeDisable(
                    impl_->connection, statements, mutation, deadline);
                break;
            case risk::BlacklistMutationOperation::ClearAll:
                succeeded = executeClear(
                    impl_->connection, statements, deadline);
                break;
            }
            if (!succeeded) {
                return false;
            }
        }
        return transaction.commit(deadline);
    } catch (...) {
        return false;
    }
}

BlacklistCounts MysqlDao::countEnabledBlacklists(
    const MysqlDeadline deadline
) {
    BlacklistCounts counts;
    impl_->last_blacklist_count_succeeded = false;
    if (impl_->connection == nullptr || deadlineReached(deadline)) {
        return counts;
    }

    static constexpr const char* kSql = R"SQL(
SELECT
    (SELECT COUNT(*) FROM risk_blacklist_user WHERE enabled = TRUE),
    (SELECT COUNT(*) FROM risk_blacklist_ip WHERE enabled = TRUE),
    (SELECT COUNT(*) FROM risk_blacklist_device WHERE enabled = TRUE)
)SQL";
    if (mysql_query(impl_->connection, kSql) != 0 ||
        deadlineReached(deadline)) {
        return counts;
    }

    using ResultPtr = std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>;
    ResultPtr result(mysql_store_result(impl_->connection), mysql_free_result);
    if (result == nullptr || deadlineReached(deadline)) {
        return counts;
    }
    MYSQL_ROW row = mysql_fetch_row(result.get());
    if (deadlineReached(deadline)) {
        return counts;
    }
    const unsigned long* lengths = mysql_fetch_lengths(result.get());
    if (row == nullptr || lengths == nullptr || deadlineReached(deadline)) {
        return counts;
    }

    const auto users = parseUInt64(row[0], lengths[0]);
    const auto ips = parseUInt64(row[1], lengths[1]);
    const auto devices = parseUInt64(row[2], lengths[2]);
    if (!users.has_value() || !ips.has_value() || !devices.has_value() ||
        deadlineReached(deadline)) {
        return counts;
    }
    counts = {*users, *ips, *devices};
    impl_->last_blacklist_count_succeeded = true;
    return counts;
}

}  // 命名空间 aegisflow::storage
