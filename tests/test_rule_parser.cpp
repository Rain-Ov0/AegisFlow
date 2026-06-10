#include "aegisflow/rule/rule_lexer.hpp"
#include "aegisflow/rule/rule_node.hpp"
#include "aegisflow/rule/rule_parser.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using aegisflow::rule::CompareOp;
using aegisflow::rule::DecisionAction;
using aegisflow::rule::NodeType;
using aegisflow::rule::RuleLexer;
using aegisflow::rule::RuleParseError;
using aegisflow::rule::RuleParser;
using aegisflow::rule::RuleSet;
using aegisflow::rule::ValueType;

RuleSet parse(const std::string& input) {
    RuleLexer lexer(input);
    RuleParser parser(lexer.tokenize());
    return parser.parseRuleSet();
}

void assertChildren(
    const RuleSet& rule_set,
    uint32_t node_id,
    const std::vector<uint32_t>& expected
) {
    assert(node_id < rule_set.nodes.size());
    assert(rule_set.nodes[node_id].children == expected);
}

void test_parse_single_condition_rule() {
    const RuleSet rule_set = parse(
        "RULE login_fail_review\n"
        "SCENE login\n"
        "PRIORITY 100\n"
        "IF user.login_fail_5m >= 5\n"
        "THEN REVIEW REASON \"too_many_failed_login\"\n"
    );

    assert(rule_set.rules.size() == 1);
    assert(rule_set.nodes.size() == 1);

    const auto& rule = rule_set.rules[0];
    assert(rule.id == 1);
    assert(rule.name == "login_fail_review");
    assert(rule.scene == "login");
    assert(rule.priority == 100);
    assert(rule.action == DecisionAction::Review);
    assert(rule.reason_code == "too_many_failed_login");
    assert(rule.root_node_id == 0);

    const auto& node = rule_set.nodes[0];
    assert(node.id == 0);
    assert(node.type == NodeType::Condition);
    assert(node.condition.feature_name == "user.login_fail_5m");
    assert(node.condition.op == CompareOp::Ge);
    assert(node.condition.expected.type == ValueType::Number);
    assert(node.condition.expected.number_value == 5);
}

void test_parse_and_condition_rule() {
    const RuleSet rule_set = parse(
        "RULE ip_attack_review\n"
        "SCENE login\n"
        "PRIORITY 90\n"
        "IF user.login_fail_5m >= 3 AND ip.distinct_user_10m >= 20\n"
        "THEN REVIEW REASON \"ip_many_users_failed_login\"\n"
    );

    assert(rule_set.rules.size() == 1);
    assert(rule_set.nodes.size() == 3);
    assert(rule_set.rules[0].root_node_id == 2);

    assert(rule_set.nodes[0].type == NodeType::Condition);
    assert(rule_set.nodes[1].type == NodeType::Condition);
    assert(rule_set.nodes[2].type == NodeType::And);
    assertChildren(rule_set, 2, {0, 1});
}

void test_parse_or_condition_rule() {
    const RuleSet rule_set = parse(
        "RULE risky_user_or_ip\n"
        "SCENE login\n"
        "PRIORITY 80\n"
        "IF user.login_fail_5m >= 5 OR ip.in_topk == true\n"
        "THEN REVIEW REASON \"risky_user_or_ip\"\n"
    );

    assert(rule_set.rules.size() == 1);
    assert(rule_set.nodes.size() == 3);
    assert(rule_set.rules[0].root_node_id == 2);
    assert(rule_set.nodes[2].type == NodeType::Or);
    assertChildren(rule_set, 2, {0, 1});
}

void test_and_has_higher_precedence_than_or() {
    const RuleSet rule_set = parse(
        "RULE precedence_rule\n"
        "SCENE login\n"
        "PRIORITY 70\n"
        "IF user.login_1m >= 1 OR user.login_5m >= 5 AND user.login_fail_5m >= 2\n"
        "THEN REVIEW REASON \"precedence_rule\"\n"
    );

    assert(rule_set.rules.size() == 1);
    assert(rule_set.nodes.size() == 5);
    assert(rule_set.rules[0].root_node_id == 4);

    assert(rule_set.nodes[3].type == NodeType::And);
    assertChildren(rule_set, 3, {1, 2});

    assert(rule_set.nodes[4].type == NodeType::Or);
    assertChildren(rule_set, 4, {0, 3});
}

void test_parse_not_and_parentheses() {
    const RuleSet rule_set = parse(
        "RULE not_parentheses_rule\n"
        "SCENE login\n"
        "PRIORITY 60\n"
        "IF NOT (user.login_fail_5m >= 5 OR ip.in_topk == true) "
        "AND device.distinct_account_10m != 0\n"
        "THEN REVIEW REASON \"not_parentheses_rule\"\n"
    );

    assert(rule_set.rules.size() == 1);
    assert(rule_set.nodes.size() == 6);
    assert(rule_set.rules[0].root_node_id == 5);

    assert(rule_set.nodes[2].type == NodeType::Or);
    assertChildren(rule_set, 2, {0, 1});

    assert(rule_set.nodes[3].type == NodeType::Not);
    assertChildren(rule_set, 3, {2});

    assert(rule_set.nodes[5].type == NodeType::And);
    assertChildren(rule_set, 5, {3, 4});
}

void test_parse_multiple_rules() {
    const RuleSet rule_set = parse(
        "RULE login_fail_review\n"
        "SCENE login\n"
        "PRIORITY 100\n"
        "IF user.login_fail_5m >= 5\n"
        "THEN REVIEW REASON \"too_many_failed_login\"\n"
        "\n"
        "RULE topk_ip_reject\n"
        "SCENE all\n"
        "PRIORITY 200\n"
        "IF ip.in_topk == true\n"
        "THEN REJECT REASON \"hot_ip\"\n"
    );

    assert(rule_set.rules.size() == 2);
    assert(rule_set.nodes.size() == 2);

    assert(rule_set.rules[0].id == 1);
    assert(rule_set.rules[1].id == 2);
    assert(rule_set.rules[1].scene == "all");
    assert(rule_set.rules[1].priority == 200);
    assert(rule_set.rules[1].action == DecisionAction::Reject);

    const auto& node = rule_set.nodes[1];
    assert(node.condition.feature_name == "ip.in_topk");
    assert(node.condition.op == CompareOp::Eq);
    assert(node.condition.expected.type == ValueType::Bool);
    assert(node.condition.expected.bool_value);
}

void test_reuse_same_condition_node() {
    const RuleSet rule_set = parse(
        "RULE r1\n"
        "SCENE login\n"
        "PRIORITY 100\n"
        "IF user.login_fail_5m >= 5\n"
        "THEN REVIEW REASON \"r1\"\n"
        "\n"
        "RULE r2\n"
        "SCENE login\n"
        "PRIORITY 90\n"
        "IF user.login_fail_5m >= 5\n"
        "THEN REJECT REASON \"r2\"\n"
    );

    assert(rule_set.rules.size() == 2);
    assert(rule_set.nodes.size() == 1);
    assert(rule_set.rules[0].root_node_id == 0);
    assert(rule_set.rules[1].root_node_id == 0);
}

void test_missing_right_parenthesis_throws_parse_error() {
    bool thrown = false;

    try {
        parse(
            "RULE bad_rule\n"
            "SCENE login\n"
            "PRIORITY 100\n"
            "IF (user.login_fail_5m >= 5 OR ip.in_topk == true\n"
            "THEN REVIEW REASON \"missing_rparen\"\n"
        );
    } catch (const RuleParseError& e) {
        thrown = true;
        const std::string message = e.what();
        assert(message.find("rule parse error at") != std::string::npos);
        assert(message.find("expected ')' after expression") != std::string::npos);
    }

    assert(thrown);
}

void test_missing_then_throws_parse_error() {
    bool thrown = false;

    try {
        parse(
            "RULE bad_rule\n"
            "SCENE login\n"
            "PRIORITY 100\n"
            "IF user.login_fail_5m >= 5\n"
            "REVIEW REASON \"missing_then\"\n"
        );
    } catch (const RuleParseError& e) {
        thrown = true;
        const std::string message = e.what();
        assert(message.find("rule parse error at") != std::string::npos);
        assert(message.find("expected THEN") != std::string::npos);
    }

    assert(thrown);
}

int main() {
    test_parse_single_condition_rule();
    test_parse_and_condition_rule();
    test_parse_or_condition_rule();
    test_and_has_higher_precedence_than_or();
    test_parse_not_and_parentheses();
    test_parse_multiple_rules();
    test_reuse_same_condition_node();
    test_missing_right_parenthesis_throws_parse_error();
    test_missing_then_throws_parse_error();

    std::cout << "test_rule_parser passed" << std::endl;
    return 0;
}