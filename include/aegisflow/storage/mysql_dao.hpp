#pragma once

#include <mysql.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aegisflow::storage {

enum class EntityType {
    User,
    Ip,
    Device
};

struct BlacklistEntry {
    EntityType type = EntityType::User;
    std::string id;
    std::string reason;
    uint64_t expire_at_ms = 0;
};

struct MysqlConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 3306;
    std::string user = "admin";
    std::string password = "123";
    std::string database = "aegisflow";
    std::string charset = "utf8mb4";

    unsigned int connect_timeout_sec = 3;
    unsigned int read_timeout_sec = 3;
    unsigned int write_timeout_sec = 3;
};

std::string entityTypeToString(EntityType type);
std::optional<EntityType> entityTypeFromString(std::string_view value);

class MysqlDao {
public:
    MysqlDao();
    explicit MysqlDao(MysqlConfig config);
    ~MysqlDao();

    MysqlDao(const MysqlDao&) = delete;
    MysqlDao& operator=(const MysqlDao&) = delete;

    bool connect();
    bool connect(const MysqlConfig& config);
    void close();

    [[nodiscard]] bool connected() const;
    [[nodiscard]] const std::string& lastError() const;

    std::vector<BlacklistEntry> loadEnabledBlacklists();

private:
    void setLastMysqlError(const std::string& prefix);

private:
    MysqlConfig config_;
    MYSQL* conn_ = nullptr;
    std::string last_error_;
};

} // namespace aegisflow::storage