#pragma once

#include "aegisflow/domain/login.hpp"
#include "aegisflow/risk/blacklist_mutation.hpp"
#include "aegisflow/risk/blacklist_snapshot.hpp"

#include <cstdint>
#include <vector>

namespace aegisflow::risk {

// 候选生成器是无状态规则对象：相同输入和 now_ms 必然产生
// 相同结果，不在此处隐藏队列、时钟或外部存储副作用。
class BlacklistCandidateGenerator final {
public:
    static constexpr std::uint64_t kTemporaryIpTtlMs =
        30ULL * 60ULL * 1000ULL;
    static constexpr const char* kCredentialStuffingReason =
        "credential_stuffing_attack";

    [[nodiscard]] std::vector<BlacklistMutation> generate(
        const aegisflow::domain::LoginAttempt& attempt,
        const aegisflow::domain::LoginDecision& decision,
        const LoginBlacklistMatches& blacklist,
        std::uint64_t now_ms
    ) const;
};

}  // 命名空间 aegisflow::risk
