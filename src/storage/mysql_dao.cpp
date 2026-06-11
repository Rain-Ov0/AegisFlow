#include "aegisflow/storage/mysql_dao.hpp"

#include <mysql.h>

#include <memory>
#include <stdexcept>
#include <utility>

namespace aegisflow::storage {

namespace {

using MysqlResultPtr = std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>;

std::string columnString(MYSQL_ROW row, const unsigned long* lengths, size_t index) {
    if (row[index] == nullptr) {
        return "";
    }

    return std::string(row[index], lengths[index]);
}

uint64_t parseUInt64(const std::string& value) {
    if (value.empty()) {
        return 0;
    }

    try {
        size_t parsed = 0;
        const uint64_t result = std::stoull(value, &parsed);
        if (parsed != value.size()) {
            return 0;
        }
        return result;
    } catch (const std::exception&) {
        return 0;
    }
}

} // namespace

std::string entityTypeToString(EntityType type) {
    switch (type) {
    case EntityType::User:
        return "user";
    case EntityType::Ip:
        return "ip";
    case EntityType::Device:
        return "device";
    }

    return "user";
}

std::optional<EntityType> entityTypeFromString(std::string_view value) {
    if (value == "user") {
        return EntityType::User;
    }
    if (value == "ip") {
        return EntityType::Ip;
    }
    if (value == "device") {
        return EntityType::Device;
    }

    return std::nullopt;
}

MysqlDao::MysqlDao() = default;

MysqlDao::MysqlDao(MysqlConfig config)
    : config_(std::move(config)) {}

MysqlDao::~MysqlDao() {
    close();
}

bool MysqlDao::connect() {
    return connect(config_);
}

bool MysqlDao::connect(const MysqlConfig& config) {
    close();

    config_ = config;
    last_error_.clear();

    MYSQL* handle = mysql_init(nullptr);
    if (handle == nullptr) {
        last_error_ = "mysql_init failed";
        return false;
    }

    mysql_options(handle, MYSQL_OPT_CONNECT_TIMEOUT, &config_.connect_timeout_sec);
    mysql_options(handle, MYSQL_OPT_READ_TIMEOUT, &config_.read_timeout_sec);
    mysql_options(handle, MYSQL_OPT_WRITE_TIMEOUT, &config_.write_timeout_sec);

    MYSQL* connected = mysql_real_connect(
        handle,
        config_.host.c_str(),
        config_.user.c_str(),
        config_.password.c_str(),
        config_.database.c_str(),
        config_.port,
        nullptr,
        0
    );

    if (connected == nullptr) {
        last_error_ = std::string("mysql_real_connect failed: ") + mysql_error(handle);
        mysql_close(handle);
        return false;
    }

    conn_ = handle;

    if (!config_.charset.empty() && mysql_set_character_set(conn_, config_.charset.c_str()) != 0) {
        setLastMysqlError("mysql_set_character_set failed");
        close();
        return false;
    }

    return true;
}

void MysqlDao::close() {
    if (conn_ != nullptr) {
        mysql_close(conn_);
        conn_ = nullptr;
    }
}

bool MysqlDao::connected() const {
    return conn_ != nullptr;
}

const std::string& MysqlDao::lastError() const {
    return last_error_;
}

std::vector<BlacklistEntry> MysqlDao::loadEnabledBlacklists() {
    std::vector<BlacklistEntry> entries;
    last_error_.clear();

    if (conn_ == nullptr) {
        last_error_ = "mysql connection is not open";
        return entries;
    }

    static constexpr const char* kSql = R"SQL(
SELECT
    entity_type,
    entity_id,
    reason,
    IFNULL(CAST(UNIX_TIMESTAMP(expire_at) * 1000 AS UNSIGNED), 0) AS expire_at_ms
FROM risk_blacklist
WHERE enabled = 1
  AND entity_type IN ('user', 'ip', 'device')
  AND (expire_at IS NULL OR expire_at > NOW())
)SQL";

    if (mysql_query(conn_, kSql) != 0) {
        setLastMysqlError("loadEnabledBlacklists query failed");
        return entries;
    }

    MysqlResultPtr result(mysql_store_result(conn_), mysql_free_result);
    if (!result) {
        if (mysql_field_count(conn_) != 0) {
            setLastMysqlError("mysql_store_result failed");
        }
        return entries;
    }

    entries.reserve(static_cast<size_t>(mysql_num_rows(result.get())));

    while (MYSQL_ROW row = mysql_fetch_row(result.get())) {
        const unsigned long* lengths = mysql_fetch_lengths(result.get());
        if (lengths == nullptr || row[0] == nullptr || row[1] == nullptr || row[2] == nullptr) {
            continue;
        }

        const std::string entity_type = columnString(row, lengths, 0);
        const auto type = entityTypeFromString(entity_type);
        if (!type.has_value()) {
            continue;
        }

        BlacklistEntry entry;
        entry.type = *type;
        entry.id = columnString(row, lengths, 1);
        entry.reason = columnString(row, lengths, 2);
        entry.expire_at_ms = row[3] == nullptr ? 0 : parseUInt64(columnString(row, lengths, 3));

        if (!entry.id.empty() && !entry.reason.empty()) {
            entries.push_back(std::move(entry));
        }
    }

    return entries;
}

void MysqlDao::setLastMysqlError(const std::string& prefix) {
    last_error_ = prefix;

    if (conn_ == nullptr) {
        return;
    }

    const char* error = mysql_error(conn_);
    if (error != nullptr && *error != '\0') {
        last_error_ += ": ";
        last_error_ += error;
    }
}

} // namespace aegisflow::storage