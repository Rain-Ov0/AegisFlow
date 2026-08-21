#pragma once

#include "aegisflow/risk/blacklist_types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace aegisflow::risk {

enum class BlacklistMutationOperation {
    Upsert,
    Disable,
    ClearAll,
};

// 所有构造都经过具名工厂，因此值对象不会出现 CLEAR_ALL 携带
// 实体、DISABLE 携带 reason 或 IP 未规范化等无效组合。
class BlacklistMutation final {
public:
    static constexpr std::size_t kMaxEntityIdBytes = 128;
    static constexpr std::size_t kMaxReasonBytes = 256;
    static constexpr std::uint64_t kMaxExpireAtMs = 253'402'300'799'999ULL;

    [[nodiscard]] static std::optional<BlacklistMutation> upsert(
        EntityType type,
        std::string id,
        std::string reason,
        std::uint64_t expire_at_ms
    );
    [[nodiscard]] static std::optional<BlacklistMutation> disable(
        EntityType type,
        std::string id
    );
    [[nodiscard]] static BlacklistMutation clearAll();

    [[nodiscard]] BlacklistMutationOperation operation() const noexcept {
        return operation_;
    }
    [[nodiscard]] std::optional<EntityType> entityType() const noexcept {
        return entity_type_;
    }
    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    [[nodiscard]] const std::string& reason() const noexcept { return reason_; }
    [[nodiscard]] std::uint64_t expireAtMs() const noexcept {
        return expire_at_ms_;
    }

    bool operator==(const BlacklistMutation& other) const {
        return operation_ == other.operation_ &&
               entity_type_ == other.entity_type_ &&
               id_ == other.id_ &&
               reason_ == other.reason_ &&
               expire_at_ms_ == other.expire_at_ms_;
    }

private:
    BlacklistMutation(
        BlacklistMutationOperation operation,
        std::optional<EntityType> entity_type,
        std::string id,
        std::string reason,
        std::uint64_t expire_at_ms
    );

    BlacklistMutationOperation operation_ = BlacklistMutationOperation::ClearAll;
    std::optional<EntityType> entity_type_;
    std::string id_;
    std::string reason_;
    std::uint64_t expire_at_ms_ = 0;
};

}  // 命名空间 aegisflow::risk
