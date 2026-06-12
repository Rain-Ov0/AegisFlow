#include "aegisflow/rule/rule_engine.hpp"
#include "aegisflow/rule/rule_lexer.hpp"
#include "aegisflow/rule/rule_parser.hpp"

#include <cassert>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using aegisflow::feature::FeatureSnapshot;
using aegisflow::rule::DecisionAction;
using aegisflow::rule::RuleEngine;
using aegisflow::rule::RuleHit;
using aegisflow::rule::RuleLexer;
using aegisflow::rule::RuleParser;
using aegisflow::rule::RuleSet;

std::shared_ptr<const RuleSet> parseRuleSet(const std::string& input) {
    RuleLexer lexer(input);
    RuleParser parser(lexer.tokenize());
    return std::make_shared<RuleSet>(parser.parseRuleSet());
}

std::vector<RuleHit> evaluate(
    const std::string& input,
    const FeatureSnapshot& features,
    const std::string& scene
) {
    RuleEngine engine(parseRuleSet(input));
    return engine.evaluate(features, scene);
}

void test_login_fail_hits_review_at_threshold() {
    FeatureSnapshot features;
    features.user_login_fail_5m = 5;

    const auto hits = evaluate(
        "RULE login_fail_review\n"
        "SCENE login\n"
        "PRIORITY 100\n"
        "IF user.login_fail_5m >= 5\n"
        "THEN REVIEW REASON \"too_many_failed_login\"\n",
        features,
        "login"
    );

    assert(hits.size() == 1);
    assert(hits[0].rule_id == 1);
    assert(hits[0].rule_name == "login_fail_review");
    assert(hits[0].priority == 100);
    assert(hits[0].action == DecisionAction::Review);
    assert(hits[0].reason_code == "too_many_failed_login");
}

void test_login_fail_below_threshold_no_hit() {
    FeatureSnapshot features;
    features.user_login_fail_5m = 4;

    const auto hits = evaluate(
        "RULE login_fail_review\n"
        "SCENE login\n"
        "PRIORITY 100\n"
        "IF user.login_fail_5m >= 5\n"
        "THEN REVIEW REASON \"too_many_failed_login\"\n",
        features,
        "login"
    );

    assert(hits.empty());
}

void test_scene_mismatch_skips_rule() {
    FeatureSnapshot features;
    features.user_login_fail_5m = 10;

    const auto hits = evaluate(
        "RULE login_fail_review\n"
        "SCENE login\n"
        "PRIORITY 100\n"
        "IF user.login_fail_5m >= 5\n"
        "THEN REVIEW REASON \"too_many_failed_login\"\n",
        features,
        "pay"
    );

    assert(hits.empty());
}

void test_all_scene_matches_any_scene() {
    FeatureSnapshot features;
    features.ip_in_topk = true;

    const auto hits = evaluate(
        "RULE topk_ip_review\n"
        "SCENE all\n"
        "PRIORITY 50\n"
        "IF ip.in_topk == true\n"
        "THEN REVIEW REASON \"hot_ip_high_frequency\"\n",
        features,
        "login"
    );

    assert(hits.size() == 1);
    assert(hits[0].reason_code == "hot_ip_high_frequency");
}

void test_and_condition_requires_all_children() {
    FeatureSnapshot features;
    features.user_login_fail_5m = 3;
    features.ip_distinct_user_10m = 20;

    const auto hits = evaluate(
        "RULE ip_attack_review\n"
        "SCENE login\n"
        "PRIORITY 90\n"
        "IF user.login_fail_5m >= 3 AND ip.distinct_user_10m >= 20\n"
        "THEN REVIEW REASON \"ip_many_users_failed_login\"\n",
        features,
        "login"
    );

    assert(hits.size() == 1);
    assert(hits[0].reason_code == "ip_many_users_failed_login");
}

void test_or_condition_matches_any_child() {
    FeatureSnapshot features;
    features.user_login_fail_5m = 0;
    features.ip_in_topk = true;

    const auto hits = evaluate(
        "RULE risky_user_or_ip\n"
        "SCENE login\n"
        "PRIORITY 80\n"
        "IF user.login_fail_5m >= 5 OR ip.in_topk == true\n"
        "THEN REVIEW REASON \"risky_user_or_ip\"\n",
        features,
        "login"
    );

    assert(hits.size() == 1);
    assert(hits[0].reason_code == "risky_user_or_ip");
}

void test_not_condition() {
    FeatureSnapshot features;
    features.ip_in_topk = false;

    const auto hits = evaluate(
        "RULE not_topk_ip\n"
        "SCENE login\n"
        "PRIORITY 60\n"
        "IF NOT (ip.in_topk == true)\n"
        "THEN REVIEW REASON \"not_topk_ip\"\n",
        features,
        "login"
    );

    assert(hits.size() == 1);
    assert(hits[0].reason_code == "not_topk_ip");
}

void test_bool_and_cms_conditions() {
    FeatureSnapshot features;
    features.ip_in_topk = true;
    features.cms_risk_behavior_count = 100;

    const auto hits = evaluate(
        "RULE topk_ip_reject\n"
        "SCENE all\n"
        "PRIORITY 200\n"
        "IF ip.in_topk == true AND cms.risk_behavior_count >= 100\n"
        "THEN REJECT REASON \"hot_ip_high_frequency\"\n",
        features,
        "login"
    );

    assert(hits.size() == 1);
    assert(hits[0].action == DecisionAction::Reject);
    assert(hits[0].reason_code == "hot_ip_high_frequency");
}

void test_blacklist_bool_conditions() {
    FeatureSnapshot features;
    features.user_black_hit = true;
    features.ip_black_hit = true;
    features.device_black_hit = false;

    const auto hits = evaluate(
        "RULE black_user_reject\n"
        "SCENE all\n"
        "PRIORITY 1000\n"
        "IF user.black_hit == true\n"
        "THEN REJECT REASON \"blacklisted_user\"\n"
        "\n"
        "RULE black_ip_reject\n"
        "SCENE all\n"
        "PRIORITY 1000\n"
        "IF ip.black_hit == true\n"
        "THEN REJECT REASON \"blacklisted_ip\"\n"
        "\n"
        "RULE black_device_reject\n"
        "SCENE all\n"
        "PRIORITY 1000\n"
        "IF device.black_hit == true\n"
        "THEN REJECT REASON \"blacklisted_device\"\n",
        features,
        "login"
    );

    assert(hits.size() == 2);
    assert(hits[0].action == DecisionAction::Reject);
    assert(hits[0].reason_code == "blacklisted_user");
    assert(hits[1].action == DecisionAction::Reject);
    assert(hits[1].reason_code == "blacklisted_ip");
}

void test_hits_sorted_by_priority_desc() {
    FeatureSnapshot features;
    features.user_login_fail_5m = 5;
    features.ip_in_topk = true;

    const auto hits = evaluate(
        "RULE low_priority_reject\n"
        "SCENE login\n"
        "PRIORITY 50\n"
        "IF ip.in_topk == true\n"
        "THEN REJECT REASON \"hot_ip\"\n"
        "\n"
        "RULE high_priority_review\n"
        "SCENE login\n"
        "PRIORITY 100\n"
        "IF user.login_fail_5m >= 5\n"
        "THEN REVIEW REASON \"too_many_failed_login\"\n",
        features,
        "login"
    );

    assert(hits.size() == 2);
    assert(hits[0].reason_code == "too_many_failed_login");
    assert(hits[0].priority == 100);
    assert(hits[1].reason_code == "hot_ip");
    assert(hits[1].priority == 50);
}

void test_unknown_feature_is_false() {
    FeatureSnapshot features;

    const auto hits = evaluate(
        "RULE unknown_feature_rule\n"
        "SCENE login\n"
        "PRIORITY 10\n"
        "IF unknown.feature >= 1\n"
        "THEN REVIEW REASON \"unknown_feature\"\n",
        features,
        "login"
    );

    assert(hits.empty());
}

int main() {
    test_login_fail_hits_review_at_threshold();
    test_login_fail_below_threshold_no_hit();
    test_scene_mismatch_skips_rule();
    test_all_scene_matches_any_scene();
    test_and_condition_requires_all_children();
    test_or_condition_matches_any_child();
    test_not_condition();
    test_bool_and_cms_conditions();
    test_blacklist_bool_conditions();
    test_hits_sorted_by_priority_desc();
    test_unknown_feature_is_false();

    std::cout << "test_rule_engine passed" << std::endl;
    return 0;
}