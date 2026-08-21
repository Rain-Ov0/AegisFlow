#pragma once

#include <cstdint>

namespace aegisflow::net {

struct ConnectionToken {
    std::uint32_t loop_id = 0;
    int fd = -1;
    std::uint64_t generation = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return fd >= 0 && generation != 0;
    }

    [[nodiscard]] constexpr bool operator==(
        const ConnectionToken& other
    ) const noexcept {
        return loop_id == other.loop_id &&
               fd == other.fd &&
               generation == other.generation;
    }
};

}  // 命名空间 aegisflow::net
