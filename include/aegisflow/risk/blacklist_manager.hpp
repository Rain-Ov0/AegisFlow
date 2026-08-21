#pragma once

#include "aegisflow/risk/blacklist_snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace aegisflow::risk {

class BlacklistSnapshotCandidate final {
public:
    BlacklistSnapshotCandidate(const BlacklistSnapshotCandidate&) = delete;
    BlacklistSnapshotCandidate& operator=(
        const BlacklistSnapshotCandidate&
    ) = delete;

private:
    BlacklistSnapshotCandidate(
        BlacklistSnapshot::EntryMap users,
        BlacklistSnapshot::EntryMap ips,
        BlacklistSnapshot::EntryMap devices
    );

    BlacklistSnapshot::EntryMap users_;
    BlacklistSnapshot::EntryMap ips_;
    BlacklistSnapshot::EntryMap devices_;

    friend class BlacklistManager;
};

class BlacklistManager final {
public:
    BlacklistManager() noexcept = default;

    BlacklistManager(const BlacklistManager&) = delete;
    BlacklistManager& operator=(const BlacklistManager&) = delete;

    [[nodiscard]] bool publish(
        const std::vector<BlacklistEntry>& entries,
        std::uint64_t now_ms
    );
    [[nodiscard]] bool publish(
        const std::vector<BlacklistEntry>& entries
    );
    [[nodiscard]] std::unique_ptr<BlacklistSnapshotCandidate> prepareCandidate(
        const std::vector<BlacklistEntry>& entries,
        std::uint64_t now_ms
    ) const noexcept;
    [[nodiscard]] bool publishCandidate(
        std::unique_ptr<BlacklistSnapshotCandidate> candidate
    ) noexcept;

    [[nodiscard]] LoginBlacklistMatches matches(
        std::string_view user_id,
        std::string_view ip,
        std::string_view device_id,
        std::uint64_t now_ms
    ) const;
private:
    [[nodiscard]] static std::uint64_t nowMillis();
    [[nodiscard]] std::shared_ptr<const BlacklistSnapshot>
        currentSnapshot() const noexcept;

    std::shared_ptr<const BlacklistSnapshot> snapshot_;
    mutable std::mutex publish_mutex_;
};

}  // 命名空间 aegisflow::risk
