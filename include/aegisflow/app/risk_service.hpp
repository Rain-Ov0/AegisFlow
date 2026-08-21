#pragma once

#include "aegisflow/domain/login.hpp"
#include "aegisflow/feature/feature_store.hpp"
#include "aegisflow/risk/blacklist_candidate_generator.hpp"
#include "aegisflow/risk/login_policy_chain.hpp"
#include "aegisflow/risk/risk_evaluation.hpp"

#include <memory>

namespace aegisflow::risk {
class BlacklistManager;
}

namespace aegisflow::app {

class RiskService {
public:
    explicit RiskService(
        aegisflow::risk::LoginPolicyConfig policy_config = {},
        const aegisflow::risk::BlacklistManager* blacklist_manager = nullptr,
        aegisflow::feature::FeatureStateReclamationConfig
            reclamation_config = {}
    );

    [[nodiscard]] aegisflow::risk::RiskEvaluation evaluate(
        const aegisflow::domain::LoginAttempt& attempt
    );

    [[nodiscard]] aegisflow::feature::LoginFeatureStore& featureStore(
    ) const noexcept {
        return *feature_store_;
    }

private:
    aegisflow::risk::LoginPolicyChain policy_chain_;
    aegisflow::risk::BlacklistCandidateGenerator candidate_generator_;
    const aegisflow::risk::BlacklistManager* blacklist_manager_ = nullptr;
    std::shared_ptr<aegisflow::feature::LoginFeatureStore> feature_store_;
};

}  // 命名空间 aegisflow::app
