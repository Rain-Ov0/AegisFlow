#include "aegisflow/risk/blacklist_manager.hpp"

#include "aegisflow/storage/mysql_dao.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace aegisflow::risk {

namespace {
EntityType convertEntityType(aegisflow::storage::EntityType type) {
    switch (type) {
    case aegisflow::storage::EntityType::User:
        return EntityType::User;
    case aegisflow::storage::EntityType::Ip:
        return EntityType::Ip;
    case aegisflow::storage::EntityType::Device:
        return EntityType::Device;
    }

    return EntityType::User;
}

BlacklistEntry convertEntry(const aegisflow::storage::BlacklistEntry& entry) {
    return {
        convertEntityType(entry.type),
        entry.id,
        entry.reason,
        entry.expire_at_ms,
    };
}

BlacklistCheckResult makeMiss(EntityType type, const std::string& id) {
    BlacklistCheckResult result;
    result.type = type;
    result.id = id;
    return result;
}
}

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

BlacklistManager::BlacklistManager(
    aegisflow::storage::MysqlDao* mysql,
    BlacklistManagerOptions options
)
    : mysql_(mysql),
      options_(options),
      bloom_(std::make_shared<aegisflow::cache::BloomFilter>(
          options_.bloom_bits,
          options_.bloom_hashes
      )),
      result_cache_(options_.cache_capacity) {}

bool BlacklistManager::loadFromMysql() {
    if (mysql_ == nullptr || !mysql_->connected()) {
        return false;
    }

    const auto storage_entries = mysql_->loadEnabledBlacklists();
    if (!mysql_->lastError().empty()) {
        return false;
    }

    std::vector<BlacklistEntry> entries;
    entries.reserve(storage_entries.size());

    for (const auto& entry : storage_entries) {
        entries.push_back(convertEntry(entry));
    }

    loadEntries(entries);
    return true;
}

void BlacklistManager::loadEntries(const std::vector<BlacklistEntry>& entries) {
    const uint64_t now_ms = nowMillis();

    auto new_bloom = std::make_shared<aegisflow::cache::BloomFilter>(
        options_.bloom_bits,
        options_.bloom_hashes
    );

    std::unordered_map<std::string, BlacklistEntry> new_blacklist;
    new_blacklist.reserve(entries.size());

    for (const auto& entry : entries) {
        if (entry.id.empty() || entry.reason.empty() || isExpired(entry, now_ms)) {
            continue;
        }

        const std::string key = makeKey(entry.type, entry.id);
        new_bloom->add(key);
        new_blacklist[key] = entry;
    }

    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        local_blacklist_ = std::move(new_blacklist);
        bloom_ = std::move(new_bloom);
    }

    result_cache_.clear();
}

BlacklistCheckResult BlacklistManager::checkUser(const std::string& user_id) {
    return check(EntityType::User, user_id);
}

BlacklistCheckResult BlacklistManager::checkIp(const std::string& ip) {
    return check(EntityType::Ip, ip);
}

BlacklistCheckResult BlacklistManager::checkDevice(const std::string& device_id) {
    return check(EntityType::Device, device_id);
}

BlacklistCheckResult BlacklistManager::checkEvent(
    const aegisflow::v1::Event& event
) {
    auto result = checkUser(event.user_id());
    if (result.hit) {
        return result;
    }

    result = checkIp(event.ip());
    if (result.hit) {
        return result;
    }

    return checkDevice(event.device_id());
}

size_t BlacklistManager::localSize() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return local_blacklist_.size();
}

BlacklistCheckResult BlacklistManager::check(
    EntityType type,
    const std::string& id
) {
    if (id.empty()) {
        return makeMiss(type, id);
    }

    const std::string key = makeKey(type, id);

    BlacklistCheckResult cached;
    if (result_cache_.get(key, cached)) {
        return cached;
    }

    const uint64_t now_ms = nowMillis();

    {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        if (!bloom_->possiblyContains(key)) {
            auto miss = makeMiss(type, id);
            result_cache_.put(key, miss, options_.negative_ttl_ms);
            return miss;
        }

        const auto it = local_blacklist_.find(key);
        if (it != local_blacklist_.end() && !isExpired(it->second, now_ms)) {
            BlacklistCheckResult hit;
            hit.hit = true;
            hit.type = it->second.type;
            hit.id = it->second.id;
            hit.reason = it->second.reason;

            result_cache_.put(key, hit, positiveCacheTtl(it->second, now_ms));
            return hit;
        }
    }

    auto miss = makeMiss(type, id);
    result_cache_.put(key, miss, options_.negative_ttl_ms);
    return miss;
}

std::string BlacklistManager::makeKey(EntityType type, const std::string& id) const {
    std::string key = entityTypeToString(type);
    key.push_back(':');
    key.append(id);
    return key;
}

bool BlacklistManager::isExpired(
    const BlacklistEntry& entry,
    uint64_t now_ms
) const {
    return entry.expire_at_ms != 0 && entry.expire_at_ms <= now_ms;
}

uint64_t BlacklistManager::positiveCacheTtl(
    const BlacklistEntry& entry,
    uint64_t now_ms
) const {
    if (entry.expire_at_ms == 0) {
        return options_.positive_ttl_ms;
    }

    if (entry.expire_at_ms <= now_ms) {
        return 0;
    }

    return std::min(options_.positive_ttl_ms, entry.expire_at_ms - now_ms);
}

uint64_t BlacklistManager::nowMillis() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()
        ).count()
    );
}

} // namespace aegisflow::risk