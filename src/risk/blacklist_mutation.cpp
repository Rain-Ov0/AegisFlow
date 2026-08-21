#include "aegisflow/risk/blacklist_mutation.hpp"

#include "aegisflow/domain/ip_address.hpp"

#include <string_view>
#include <utility>

namespace aegisflow::risk {
namespace {

bool isKnownType(const EntityType type) noexcept {
    switch (type) {
    case EntityType::User:
    case EntityType::Ip:
    case EntityType::Device:
        return true;
    }
    return false;
}

bool validText(
    const std::string_view value,
    const std::size_t max_bytes
) noexcept {
    return !value.empty() && value.size() <= max_bytes &&
           value.find('\0') == std::string_view::npos;
}

std::optional<std::string> normalizeId(
    const EntityType type,
    std::string id
) {
    if (!isKnownType(type)) {
        return std::nullopt;
    }
    if (type != EntityType::Ip) {
        if (!validText(id, BlacklistMutation::kMaxEntityIdBytes)) {
            return std::nullopt;
        }
        return id;
    }

    const auto address = domain::IpAddress::parse(id);
    if (!address.has_value()) {
        return std::nullopt;
    }
    return address->canonicalText();
}

}  // 命名空间

BlacklistMutation::BlacklistMutation(
    const BlacklistMutationOperation operation,
    std::optional<EntityType> entity_type,
    std::string id,
    std::string reason,
    const std::uint64_t expire_at_ms
)
    : operation_(operation),
      entity_type_(entity_type),
      id_(std::move(id)),
      reason_(std::move(reason)),
      expire_at_ms_(expire_at_ms) {}

std::optional<BlacklistMutation> BlacklistMutation::upsert(
    const EntityType type,
    std::string id,
    std::string reason,
    const std::uint64_t expire_at_ms
) {
    auto normalized_id = normalizeId(type, std::move(id));
    if (!normalized_id.has_value() ||
        !validText(reason, kMaxReasonBytes) ||
        expire_at_ms > kMaxExpireAtMs) {
        return std::nullopt;
    }
    return BlacklistMutation(
        BlacklistMutationOperation::Upsert,
        type,
        std::move(*normalized_id),
        std::move(reason),
        expire_at_ms
    );
}

std::optional<BlacklistMutation> BlacklistMutation::disable(
    const EntityType type,
    std::string id
) {
    auto normalized_id = normalizeId(type, std::move(id));
    if (!normalized_id.has_value()) {
        return std::nullopt;
    }
    return BlacklistMutation(
        BlacklistMutationOperation::Disable,
        type,
        std::move(*normalized_id),
        "",
        0
    );
}

BlacklistMutation BlacklistMutation::clearAll() {
    return BlacklistMutation(
        BlacklistMutationOperation::ClearAll,
        std::nullopt,
        "",
        "",
        0
    );
}

}  // 命名空间 aegisflow::risk
