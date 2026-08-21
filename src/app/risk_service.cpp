#include "aegisflow/app/risk_service.hpp"

#include "aegisflow/risk/blacklist_manager.hpp"

#include <chrono>
#include <cstdint>
#include <utility>

namespace aegisflow::app {
namespace {

std::uint64_t nowMills() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()
        ).count()
    );
}

std::uint64_t elapsedMicros(
    const std::chrono::steady_clock::time_point start
) {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<microseconds>(steady_clock::now() - start).count()
    );
}

}  // 命名空间

RiskService::RiskService(
    aegisflow::risk::LoginPolicyConfig policy_config,
    const aegisflow::risk::BlacklistManager* blacklist_manager,
    aegisflow::feature::FeatureStateReclamationConfig reclamation_config
)
    : policy_chain_(policy_config),
      blacklist_manager_(blacklist_manager),
      feature_store_(
          std::make_shared<aegisflow::feature::LoginFeatureStore>(
              reclamation_config
          )
      ) {}

aegisflow::risk::RiskEvaluation RiskService::evaluate(
    const aegisflow::domain::LoginAttempt& attempt
) {
    const auto start = std::chrono::steady_clock::now();
    const std::uint64_t now_ms = nowMills();

    // 特征先纳入当前请求，使阈值在第 N 次失败当次立即生效。
    const auto features = feature_store_->updateAndGet(attempt, now_ms);

    aegisflow::risk::LoginBlacklistMatches blacklist;
    if (blacklist_manager_ != nullptr) {
        // 查询只读一份不可变快照，不与 MySQL 刷新线程共享可变容器。
        blacklist = blacklist_manager_->matches(
            attempt.userId(),
            attempt.ip(),
            attempt.deviceId(),
            now_ms
        );
    }

    // 策略链保留全部命中，最终动作再按严重度聚合。
    auto decision = policy_chain_.evaluate(attempt, features, blacklist);
    auto candidates = candidate_generator_.generate(
        attempt,
        decision,
        blacklist,
        now_ms
    );
    decision.cost_us = elapsedMicros(start);
    aegisflow::risk::RiskEvaluation evaluation;
    evaluation.decision = std::move(decision);
    evaluation.candidates = std::move(candidates);
    return evaluation;
}

}  // 命名空间 aegisflow::app
