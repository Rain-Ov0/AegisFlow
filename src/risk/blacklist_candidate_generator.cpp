#include "aegisflow/risk/blacklist_candidate_generator.hpp"

#include <algorithm>
#include <limits>
#include <string_view>

namespace aegisflow::risk {

std::vector<BlacklistMutation> BlacklistCandidateGenerator::generate(
    const aegisflow::domain::LoginAttempt& attempt,
    const aegisflow::domain::LoginDecision& decision,
    const LoginBlacklistMatches& blacklist,
    const std::uint64_t now_ms
) const {
    if (decision.action != aegisflow::domain::LoginDecisionAction::reject ||
        blacklist.user_hit || blacklist.ip_hit || blacklist.device_hit) {
        return {};
    }

    const bool credential_stuffing_reject = std::any_of(
        decision.policy_hits.begin(),
        decision.policy_hits.end(),
        [](const aegisflow::domain::PolicyHit& hit) {
            return hit.action ==
                       aegisflow::domain::LoginDecisionAction::reject &&
                   hit.reason_code == kCredentialStuffingReason;
        }
    );
    if (!credential_stuffing_reject ||
        now_ms > BlacklistMutation::kMaxExpireAtMs - kTemporaryIpTtlMs) {
        return {};
    }

    auto mutation = BlacklistMutation::upsert(
        EntityType::Ip,
        attempt.ip(),
        kCredentialStuffingReason,
        now_ms + kTemporaryIpTtlMs
    );
    if (!mutation.has_value()) {
        return {};
    }

    std::vector<BlacklistMutation> candidates;
    candidates.reserve(1);
    candidates.push_back(std::move(*mutation));
    return candidates;
}

}  // 命名空间 aegisflow::risk
