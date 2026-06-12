#include "aegisflow/rule/rule_lexer.hpp"
#include "aegisflow/rule/rule_parser.hpp"
#include "aegisflow/rule/rule_validator.hpp"

#include <cassert>
#include <iostream>
#include <string>

using aegisflow::rule::RuleLexer;
using aegisflow::rule::RuleParser;
using aegisflow::rule::RuleSet;
using aegisflow::rule::RuleValidationError;
using aegisflow::rule::RuleValidator;

RuleSet parse(const std::string& input) {
    RuleLexer lexer(input);
    RuleParser parser(lexer.tokenize());
    return parser.parseRuleSet();
}

void assertValidationMessage(
    const std::string& input,
    const std::string& expected
) {
    bool thrown = false;

    try {
        RuleValidator::validate(parse(input));
    } catch (const RuleValidationError& e) {
        thrown = true;
        const std::string message = e.what();
        assert(message.find(expected) != std::string::npos);
    }

    assert(thrown);
}

void test_valid_rule_set_passes() {
    const RuleSet rule_set = parse(
        "RULE black_user_reject\n"
        "SCENE all\n"
        "PRIORITY 1000\n"
        "IF user.black_hit == true\n"
        "THEN REJECT REASON \"blacklisted_user\"\n"
        "\n"
        "RULE login_fail_review\n"
        "SCENE login\n"
        "PRIORITY 100\n"
        "IF user.login_fail_5m >= 5\n"
        "THEN REVIEW REASON \"too_many_failed_login\"\n"
        "\n"
        "RULE topk_ip_review\n"
        "SCENE all\n"
        "PRIORITY 50\n"
        "IF ip.in_topk == true AND cms.risk_behavior_count >= 100\n"
        "THEN REVIEW REASON \"hot_ip_high_frequency\"\n"
    );

    RuleValidator::validate(rule_set);
}

void test_blacklist_features_are_supported() {
    assert(RuleValidator::isSupportedFeature("user.black_hit"));
    assert(RuleValidator::isSupportedFeature("ip.black_hit"));
    assert(RuleValidator::isSupportedFeature("device.black_hit"));
}

void test_unknown_feature_reports_line_and_column() {
    assertValidationMessage(
        "RULE bad_feature\n"
        "SCENE login\n"
        "PRIORITY 10\n"
        "IF unknown.feature >= 1\n"
        "THEN REVIEW REASON \"bad_feature\"\n",
        "rule validation error at 4:4 unknown feature 'unknown.feature'"
    );
}

void test_number_feature_requires_number_value() {
    assertValidationMessage(
        "RULE bad_type\n"
        "SCENE login\n"
        "PRIORITY 10\n"
        "IF user.login_fail_5m == true\n"
        "THEN REVIEW REASON \"bad_type\"\n",
        "feature 'user.login_fail_5m' expects number value but got bool"
    );
}

void test_bool_feature_requires_bool_value() {
    assertValidationMessage(
        "RULE bad_type\n"
        "SCENE login\n"
        "PRIORITY 10\n"
        "IF ip.in_topk == 1\n"
        "THEN REVIEW REASON \"bad_type\"\n",
        "feature 'ip.in_topk' expects bool value but got number"
    );
}

void test_blacklist_bool_feature_requires_bool_value() {
    assertValidationMessage(
        "RULE bad_black_type\n"
        "SCENE all\n"
        "PRIORITY 1000\n"
        "IF user.black_hit == 1\n"
        "THEN REJECT REASON \"bad_black_type\"\n",
        "feature 'user.black_hit' expects bool value but got number"
    );
}

void test_bool_feature_rejects_ordering_operator() {
    assertValidationMessage(
        "RULE bad_operator\n"
        "SCENE login\n"
        "PRIORITY 10\n"
        "IF ip.in_topk > true\n"
        "THEN REVIEW REASON \"bad_operator\"\n",
        "feature 'ip.in_topk' only supports == and !="
    );
}

int main() {
    test_valid_rule_set_passes();
    test_blacklist_features_are_supported();
    test_unknown_feature_reports_line_and_column();
    test_number_feature_requires_number_value();
    test_bool_feature_requires_bool_value();
    test_blacklist_bool_feature_requires_bool_value();
    test_bool_feature_rejects_ordering_operator();

    std::cout << "test_rule_validator passed" << std::endl;
    return 0;
}