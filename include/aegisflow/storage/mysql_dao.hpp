#pragma once

#include "aegisflow/base/array_view.hpp"
#include "aegisflow/risk/blacklist_mutation.hpp"
#include "aegisflow/risk/blacklist_types.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aegisflow::storage {

using MysqlDeadline = std::chrono::steady_clock::time_point;

struct MysqlConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 3306;
    std::string user = "aegisflow";
    std::string password;
    std::string database = "aegisflow";
    std::string charset = "utf8mb4";

    unsigned int connect_timeout_sec = 3;
    unsigned int read_timeout_sec = 3;
    unsigned int write_timeout_sec = 3;

    bool operator==(const MysqlConfig& other) const {
        return host == other.host &&
               port == other.port &&
               user == other.user &&
               password == other.password &&
               database == other.database &&
               charset == other.charset &&
               connect_timeout_sec == other.connect_timeout_sec &&
               read_timeout_sec == other.read_timeout_sec &&
               write_timeout_sec == other.write_timeout_sec;
    }
};

struct BlacklistCounts {
    std::uint64_t users = 0;
    std::uint64_t ips = 0;
    std::uint64_t devices = 0;

    [[nodiscard]] std::uint64_t total() const noexcept {
        return users + ips + devices;
    }
    [[nodiscard]] bool empty() const noexcept { return total() == 0; }

    bool operator==(const BlacklistCounts& other) const {
        return users == other.users &&
               ips == other.ips &&
               devices == other.devices;
    }
};

class MysqlDao {
public:
    explicit MysqlDao(MysqlConfig config);
    ~MysqlDao();

    MysqlDao(const MysqlDao&) = delete;
    MysqlDao& operator=(const MysqlDao&) = delete;

    // 所有操作共用调用者给出的绝对时点。连接时把
    // mysqlclient 的整秒 connect/read/write timeout 压缩到剩余预算，
    // 后续在每个查询、statement 和事务边界前后复查。
    bool connect(MysqlDeadline deadline);
    [[nodiscard]] bool lastBlacklistLoadSucceeded() const noexcept;
    [[nodiscard]] bool lastBlacklistCountSucceeded() const noexcept;

    std::vector<risk::BlacklistEntry> loadEnabledBlacklists(
        MysqlDeadline deadline
    );
    [[nodiscard]] bool applyBlacklistMutations(
        base::ArrayView<const risk::BlacklistMutation> mutations,
        MysqlDeadline deadline
    );
    [[nodiscard]] BlacklistCounts countEnabledBlacklists(
        MysqlDeadline deadline
    );

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // 命名空间 aegisflow::storage
