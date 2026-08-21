#pragma once

#include <cstdint>
#include <string>

namespace aegisflow::risk {

enum class EntityType {
    User,
    Ip,
    Device,
};

struct BlacklistEntry {
    EntityType type = EntityType::User;
    std::string id;
    std::uint64_t expire_at_ms = 0;
};

}  // 命名空间 aegisflow::risk
