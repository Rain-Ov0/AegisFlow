#pragma once

#include "aegisflow/risk/blacklist_types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <unordered_map>

namespace aegisflow::risk {

struct LoginBlacklistMatches {
    bool user_hit = false;
    bool ip_hit = false;
    bool device_hit = false;
};

class BlacklistSnapshot final {
public:
    [[nodiscard]] LoginBlacklistMatches matches(
        std::string_view user_id,
        std::string_view ip,
        std::string_view device_id,
        std::uint64_t now_ms
    ) const;

    struct StringHash {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(
            std::string_view value
        ) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
    };

    using EntryMap = std::unordered_map<
        std::string,
        std::uint64_t,
        StringHash,
        std::equal_to<>
    >;

private:
    BlacklistSnapshot(
        EntryMap users,
        EntryMap ips,
        EntryMap devices
    );

    [[nodiscard]] static bool contains(
        const EntryMap& entries,
        std::string_view id,
        std::uint64_t now_ms
    );

    friend class BlacklistManager;

    EntryMap users_;
    EntryMap ips_;
    EntryMap devices_;
};

}  // 命名空间 aegisflow::risk
