#include "aegisflow/app/risk_service.hpp"

#include "aegisflow/rule/rule_lexer.hpp"
#include "aegisflow/rule/rule_parser.hpp"
#include "aegisflow/rule/rule_validator.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace aegisflow::app {

namespace {

uint64_t nowMills() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()
        ).count()
    );
}

uint64_t elapsedMicros(std::chrono::steady_clock::time_point start) {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(
            steady_clock::now() - start
        ).count()
    );
}

std::shared_ptr<const aegisflow::rule::RuleSet> loadRuleSetFromFile(
    const std::string& rule_file
) {
    std::ifstream input(rule_file);
    if (!input.is_open()) {
        throw std::runtime_error("failed to open rule file: " + rule_file);
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();

    try {
        aegisflow::rule::RuleLexer lexer(buffer.str());
        aegisflow::rule::RuleParser parser(lexer.tokenize());

        auto rule_set = parser.parseRuleSet();
        aegisflow::rule::RuleValidator::validate(rule_set);

        return std::make_shared<aegisflow::rule::RuleSet>(std::move(rule_set));
    } catch (const std::exception& e) {
        throw std::runtime_error(
            "failed to load rule file '" + rule_file + "': " + e.what()
        );
    }
}

std::string formatRecentAction(const aegisflow::feature::RecentAction& action) {
    return aegisflow::v1::EventType_Name(action.type) +
        ":" +
        aegisflow::v1::EventResult_Name(action.result) +
        "@" +
        std::to_string(action.timestamp_ms);
}

void fillFeatureSnapshot(
    const aegisflow::feature::FeatureSnapshot& snapshot,
    aegisflow::v1::FeatureSnapshot* pb_snapshot
) {
    pb_snapshot->set_user_id(snapshot.user_id);
    pb_snapshot->set_user_login_1m(snapshot.user_login_1m);
    pb_snapshot->set_user_login_5m(snapshot.user_login_5m);
    pb_snapshot->set_user_login_1h(snapshot.user_login_1h);
    pb_snapshot->set_user_login_fail_5m(snapshot.user_login_fail_5m);

    for (const auto& action : snapshot.recent_actions) {
        pb_snapshot->add_recent_actions(formatRecentAction(action));
    }

    pb_snapshot->set_ip_distinct_user_10m(snapshot.ip_distinct_user_10m);
    pb_snapshot->set_device_distinct_account_10m(snapshot.device_distinct_account_10m);
    pb_snapshot->set_cms_risk_behavior_count(snapshot.cms_risk_behavior_count);
    pb_snapshot->set_ip_topk_estimated_count(snapshot.ip_topk_estimated_count);
    pb_snapshot->set_ip_in_topk(snapshot.ip_in_topk);
}

size_t normalizeWorkerNum(size_t worker_num) {
    if (worker_num != 0) {
        return worker_num;
    }

    const unsigned int hardware_num = std::thread::hardware_concurrency();
    if (hardware_num == 0) {
        return 4;
    }

    return std::max<size_t>(2, hardware_num);
}

aegisflow::v1::DecisionAction toProtoAction(
    aegisflow::rule::DecisionAction action
) {
    switch (action) {
    case aegisflow::rule::DecisionAction::Pass:
        return aegisflow::v1::PASS;
    case aegisflow::rule::DecisionAction::Review:
        return aegisflow::v1::REVIEW;
    case aegisflow::rule::DecisionAction::Reject:
        return aegisflow::v1::REJECT;
    }

    return aegisflow::v1::DECISION_ACTION_UNKOWN;
}

void appendValidationHits(
    const aegisflow::v1::Event& event,
    std::vector<aegisflow::rule::RuleHit>& hits
) {
    if (!event.user_id().empty()) {
        return;
    }

    hits.push_back({
        0,
        "request_validation",
        100000,
        aegisflow::rule::DecisionAction::Review,
        "invalid_user_id"
    });
}

void fillReasons(
    const aegisflow::rule::DecisionResult& result,
    aegisflow::v1::Decision* decision
) {
    for (const auto& reason_code : result.reasons) {
        auto* reason = decision->add_reasons();
        reason->set_code(reason_code);
        reason->set_message(reason_code);
        reason->set_severity(1);
    }
}

} // namespace

RiskService::RiskService(size_t worker_num, std::string rule_file)
    : rule_set_(loadRuleSetFromFile(rule_file)),
      rule_engine_(rule_set_),
      decision_aggregator_(),
      feature_store_(),
      worker_pool_(normalizeWorkerNum(worker_num)) {}

aegisflow::v1::ReportEventResponse RiskService::handleEvent(
    const aegisflow::v1::ReportEventRequest& request
) {
    const auto start = std::chrono::steady_clock::now();
    const uint64_t now_ms = nowMills();

    aegisflow::v1::ReportEventResponse response;

    const auto& event = request.event();
    auto* decision = response.mutable_decision();

    auto future = worker_pool_.submit([this, event, now_ms]() {
        return feature_store_.updateAndGet(event, now_ms);
    });

    const auto snapshot = future.get();

    auto hits = rule_engine_.evaluate(snapshot, event.scene());
    appendValidationHits(event, hits);

    const auto result = decision_aggregator_.aggregate(hits);

    decision->set_event_id(event.event_id());
    decision->set_user_id(event.user_id());
    decision->set_action(toProtoAction(result.action));
    decision->set_risk_score(result.risk_score);

    fillReasons(result, decision);
    fillFeatureSnapshot(snapshot, decision->mutable_features());

    decision->set_cost_us(elapsedMicros(start));

    return response;
}

} // namespace aegisflow::app