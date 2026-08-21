#include "aegisflow/risk/blacklist_snapshot.hpp"

#include <utility>

namespace aegisflow::risk {

BlacklistSnapshot::BlacklistSnapshot(
    EntryMap users,
    EntryMap ips,
    EntryMap devices
)
    : users_(std::move(users)),
      ips_(std::move(ips)),
      devices_(std::move(devices)) {}

LoginBlacklistMatches BlacklistSnapshot::matches(
    const std::string_view user_id,
    const std::string_view ip,
    const std::string_view device_id,
    const std::uint64_t now_ms
) const {
    LoginBlacklistMatches matches;
    matches.user_hit = contains(users_, user_id, now_ms);
    matches.ip_hit = contains(ips_, ip, now_ms);
    matches.device_hit = contains(devices_, device_id, now_ms);
    return matches;
}

bool BlacklistSnapshot::contains(
    const EntryMap& entries,
    const std::string_view id,
    const std::uint64_t now_ms
) {
    if (id.empty()) {
        return false;
    }

    const auto entry = entries.find(std::string(id));
    if (entry == entries.end()) {
        return false;
    }
    // 刷新周期内也在读取时判断过期，避免已到期条目继续命中。
    return entry->second == 0 || entry->second > now_ms;
}

}  // 命名空间 aegisflow::risk
