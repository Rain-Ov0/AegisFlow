#include "aegisflow/rule/rule_lexer.hpp"
#include "aegisflow/rule/rule_node.hpp"
#include "aegisflow/rule/rule_parser.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>

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
    test_parse_multiple_rules();
    test_reuse_same_condition_node();
    test_missing_then_throws_parse_error();

    std::cout << "test_rule_parser passed" << std::endl;
    return 0;
}