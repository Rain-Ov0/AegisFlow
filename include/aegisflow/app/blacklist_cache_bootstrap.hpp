#pragma once

#include "aegisflow/storage/blacklist_redis_store.hpp"
#include "aegisflow/storage/mysql_dao.hpp"

#include <cstddef>
#include <chrono>

namespace aegisflow::app {

struct BlacklistCacheBootstrapConfig {
    std::chrono::milliseconds startup_timeout{10'000};
    std::size_t batch_size = 100;
    std::chrono::milliseconds reset_timeout{10'000};

    bool operator==(const BlacklistCacheBootstrapConfig& other) const {
        return startup_timeout == other.startup_timeout &&
               batch_size == other.batch_size &&
               reset_timeout == other.reset_timeout;
    }
};

struct BlacklistBootstrapResult {
    storage::StoreStatus status = storage::StoreStatus::IoError;
    storage::SnapshotReadResult snapshot;
};

// ready 缺失时先按 Stream 顺序收敛 MySQL，再原子重建 Redis。
// 协调层拥有顺序，Store 不持有 MysqlDao 或跨越存储边界的事务。
[[nodiscard]] BlacklistBootstrapResult initializeBlacklistCache(
    storage::BlacklistRedisStore& redis,
    storage::MysqlDao& mysql,
    std::size_t batch_size,
    std::size_t scan_count,
    storage::RedisDeadline deadline
);

}  // 命名空间 aegisflow::app
