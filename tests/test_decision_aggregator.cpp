#include "aegisflow/rule/decision_aggregator.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using aegisflow::rule::DecisionAction;
using aegisflow::rule::DecisionAggregator;
using aegisflow::rule::RuleHit;

RuleHit makeHit(
    uint64_t rule_id,
    const std::string& rule_name,
    int priority,
    DecisionAction action,
    const std::string& reason_code
) {
    return {
        rule_id,
        rule_name,
        priority,
        action,
        reason_code
    };
}

void test_no_hits_returns_pass() {
    DecisionAggregator aggregator;

    const auto result = aggregator.aggregate({});

    assert(result.action == DecisionAction::Pass);
    assert(result.risk_score == 0);
    assert(result.hit_rule_ids.empty());
    assert(result.reasons.empty());
}

void test_single_review_hit() {
    DecisionAggregator aggregator;

    const auto result = aggregator.aggregate({
        makeHit(
            1,
            "login_fail_review",
            100,
            DecisionAction::Review,
            "too_many_failed_login"
        )
    });

    assert(result.action == DecisionAction::Review);
    assert(result.risk_score == 30);
    assert(result.hit_rule_ids.size() == 1);
    assert(result.hit_rule_ids[0] == 1);
    assert(result.reasons.size() == 1);
    assert(result.reasons[0] == "too_many_failed_login");
}

void test_reject_overrides_review_and_scores_accumulate() {
    DecisionAggregator aggregator;

    const auto result = aggregator.aggregate({
        makeHit(
            1,
            "login_fail_review",
            100,
            DecisionAction::Review,
            "too_many_failed_login"
        ),
        makeHit(
            2,
            "hot_ip_reject",
            90,
            DecisionAction::Reject,
            "hot_ip_high_frequency"
        )
    });

    assert(result.action == DecisionAction::Reject);
    assert(result.risk_score == 130);

    assert(result.hit_rule_ids.size() == 2);
    assert(result.hit_rule_ids[0] == 1);
    assert(result.hit_rule_ids[1] == 2);

    assert(result.reasons.size() == 2);
    assert(result.reasons[0] == "too_many_failed_login");
    assert(result.reasons[1] == "hot_ip_high_frequency");
}

void test_pass_hit_keeps_reason_but_does_not_increase_score() {
    DecisionAggregator aggregator;

    const auto result = aggregator.aggregate({
        makeHit(
            3,
            "allow_low_risk",
            10,
            DecisionAction::Pass,
            "allow_low_risk"
        )
    });

    assert(result.action == DecisionAction::Pass);
    assert(result.risk_score == 0);
    assert(result.hit_rule_ids.size() == 1);
    assert(result.hit_rule_ids[0] == 3);
    assert(result.reasons.size() == 1);
    assert(result.reasons[0] == "allow_low_risk");
}

int main() {
    test_no_hits_returns_pass();
    test_single_review_hit();
    test_reject_overrides_review_and_scores_accumulate();
    test_pass_hit_keeps_reason_but_does_not_increase_score();

    std::cout << "test_decision_aggregator passed" << std::endl;
    return 0;
}