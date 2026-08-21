#include "aegisflow/risk/blacklist_manager.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>

namespace aegisflow::risk {
namespace {

using EntryMap = BlacklistSnapshot::EntryMap;

bool mergeExpiry(
    EntryMap& entries,
    const std::string& id,
    const std::uint64_t expire_at_ms
) {
    const auto [entry, inserted] = entries.try_emplace(id, expire_at_ms);
    if (inserted) {
        return true;
    }

    if (entry->second == 0 || expire_at_ms == 0) {
        // 同一实体重复出现时，永久条目优先，否则保留更晚的过期时间。
        entry->second = 0;
    } else {
        entry->second = std::max(entry->second, expire_at_ms);
    }
    return true;
}

bool addEntry(
    EntryMap& users,
    EntryMap& ips,
    EntryMap& devices,
    const BlacklistEntry& entry,
    const std::uint64_t now_ms
) {
    if (entry.id.empty()) {
        return false;
    }

    if (entry.expire_at_ms != 0 && entry.expire_at_ms <= now_ms) {
        return true;
    }

    switch (entry.type) {
    case EntityType::User:
        return mergeExpiry(users, entry.id, entry.expire_at_ms);
    case EntityType::Ip:
        return mergeExpiry(ips, entry.id, entry.expire_at_ms);
    case EntityType::Device:
        return mergeExpiry(devices, entry.id, entry.expire_at_ms);
    }
    return false;
}

}  // 命名空间

BlacklistSnapshotCandidate::BlacklistSnapshotCandidate(
    BlacklistSnapshot::EntryMap users,
    BlacklistSnapshot::EntryMap ips,
    BlacklistSnapshot::EntryMap devices
) : users_(std::move(users)),
    ips_(std::move(ips)),
    devices_(std::move(devices)) {}

bool BlacklistManager::publish(
    const std::vector<BlacklistEntry>& entries,
    const std::uint64_t now_ms
) {
    auto candidate = prepareCandidate(entries, now_ms);
    if (candidate == nullptr) {
        return false;
    }
    return publishCandidate(std::move(candidate));
}

std::unique_ptr<BlacklistSnapshotCandidate>
BlacklistManager::prepareCandidate(
    const std::vector<BlacklistEntry>& entries,
    const std::uint64_t now_ms
) const noexcept {
    std::array<std::size_t, 3> entry_counts{};
    for (const auto& entry : entries) {
        if (entry.id.empty()) {
            return nullptr;
        }
        if (entry.expire_at_ms != 0 && entry.expire_at_ms <= now_ms) {
            continue;
        }

        switch (entry.type) {
        case EntityType::User:
            ++entry_counts[0];
            break;
        case EntityType::Ip:
            ++entry_counts[1];
            break;
        case EntityType::Device:
            ++entry_counts[2];
            break;
        default:
            return nullptr;
        }
    }

    try {
        // 候选快照在发布锁外完整构造，读线程只会看到发布前或发布后的完整快照。
        EntryMap users;
        EntryMap ips;
        EntryMap devices;
        users.reserve(entry_counts[0]);
        ips.reserve(entry_counts[1]);
        devices.reserve(entry_counts[2]);

        for (const auto& entry : entries) {
            if (!addEntry(users, ips, devices, entry, now_ms)) {
                return nullptr;
            }
        }

        return std::unique_ptr<BlacklistSnapshotCandidate>(
            new BlacklistSnapshotCandidate(
                std::move(users),
                std::move(ips),
                std::move(devices)
            )
        );
    } catch (...) {
        return nullptr;
    }
}

bool BlacklistManager::publishCandidate(
    std::unique_ptr<BlacklistSnapshotCandidate> candidate
) noexcept {
    if (candidate == nullptr) {
        return false;
    }
    try {
        std::shared_ptr<const BlacklistSnapshot> snapshot(
            new BlacklistSnapshot(
                std::move(candidate->users_),
                std::move(candidate->ips_),
                std::move(candidate->devices_)
            )
        );
        std::lock_guard lock(publish_mutex_);
        // release/acquire 发布不可变对象，请求热路径无需获取发布锁。
        std::atomic_store_explicit(
            &snapshot_, std::move(snapshot), std::memory_order_release);
        return true;
    } catch (...) {
        return false;
    }
}

bool BlacklistManager::publish(
    const std::vector<BlacklistEntry>& entries
) {
    return publish(entries, nowMillis());
}

LoginBlacklistMatches BlacklistManager::matches(
    const std::string_view user_id,
    const std::string_view ip,
    const std::string_view device_id,
    const std::uint64_t now_ms
) const {
    const auto snapshot = currentSnapshot();
    if (snapshot == nullptr) {
        return {};
    }
    return snapshot->matches(user_id, ip, device_id, now_ms);
}

std::shared_ptr<const BlacklistSnapshot>
BlacklistManager::currentSnapshot() const noexcept {
    return std::atomic_load_explicit(
        &snapshot_, std::memory_order_acquire);
}

std::uint64_t BlacklistManager::nowMillis() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()
        ).count()
    );
}

}  // 命名空间 aegisflow::risk
