#pragma once

#include "aegisflow/cache/bloom_filter.hpp"
#include "aegisflow/cache/ttl_cache.hpp"
#include "event.pb.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace aegisflow::storage {
class MysqlDao;
class RedisClient;
}

namespace aegisflow::risk {

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

struct BlacklistCheckResult {
    bool hit = false;
    EntityType type = EntityType::User;
    std::string id;
    std::string reason;
};

struct BlacklistManagerOptions {
    size_t bloom_bits = 1 << 24;
    size_t bloom_hashes = 7;
    size_t cache_capacity = 100000;
    uint64_t positive_ttl_ms = 60ULL * 1000ULL;
    uint64_t negative_ttl_ms = 30ULL * 1000ULL;
};

std::string entityTypeToString(EntityType type);

class BlacklistManager {
public:
    explicit BlacklistManager(
        aegisflow::storage::MysqlDao* mysql = nullptr,
        BlacklistManagerOptions options = {}
    );

    BlacklistManager(
        aegisflow::storage::MysqlDao* mysql,
        aegisflow::storage::RedisClient* redis,
        BlacklistManagerOptions options = {}
    );

    bool loadFromMysql();
    void loadEntries(const std::vector<BlacklistEntry>& entries);

    BlacklistCheckResult checkUser(const std::string& user_id);
    BlacklistCheckResult checkIp(const std::string& ip);
    BlacklistCheckResult checkDevice(const std::string& device_id);
    BlacklistCheckResult checkEvent(const aegisflow::v1::Event& event);

    [[nodiscard]] size_t localSize() const;

private:
    BlacklistCheckResult check(EntityType type, const std::string& id);
    BlacklistCheckResult checkRedis(
        EntityType type,
        const std::string& id,
        const std::string& local_key
    );

    [[nodiscard]] std::string makeKey(EntityType type, const std::string& id) const;
    [[nodiscard]] std::string makeRedisKey(const std::string& local_key) const;
    [[nodiscard]] bool isExpired(const BlacklistEntry& entry, uint64_t now_ms) const;
    [[nodiscard]] uint64_t positiveCacheTtl(
        const BlacklistEntry& entry,
        uint64_t now_ms
    ) const;

    static uint64_t nowMillis();

private:
    aegisflow::storage::MysqlDao* mysql_ = nullptr;
    aegisflow::storage::RedisClient* redis_ = nullptr;
    BlacklistManagerOptions options_;

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, BlacklistEntry> local_blacklist_;
    std::shared_ptr<aegisflow::cache::BloomFilter> bloom_;

    aegisflow::cache::TtlLruCache<std::string, BlacklistCheckResult> result_cache_;
};

} // namespace aegisflow::risk