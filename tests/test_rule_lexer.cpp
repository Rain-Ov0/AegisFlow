#include "aegisflow/rule/rule_lexer.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using aegisflow::rule::RuleLexer;
using aegisflow::rule::Token;
using aegisflow::rule::TokenType;

void assertToken(
    const std::vector<Token>& tokens,
    size_t index,
    TokenType type,
    const std::string& text
) {
    assert(index < tokens.size());
    assert(tokens[index].type == type);
    assert(tokens[index].text == text);
}

void test_single_rule_tokens() {
    const std::string input =
        "RULE login_fail_review\n"
        "SCENE login\n"
        "PRIORITY 100\n"
        "IF user.login_fail_5m >= 5\n"
        "THEN REVIEW REASON \"too_many_failed_login\"\n";

    RuleLexer lexer(input);
    const auto tokens = lexer.tokenize();

    const std::vector<TokenType> expected_types = {
        TokenType::Rule,
        TokenType::Identifier,
        TokenType::Scene,
        TokenType::Identifier,
        TokenType::Priority,
        TokenType::Number,
        TokenType::If,
        TokenType::Identifier,
        TokenType::Ge,
        TokenType::Number,
        TokenType::Then,
        TokenType::Review,
        TokenType::Reason,
        TokenType::String,
        TokenType::End
    };

    assert(tokens.size() == expected_types.size());

    for (size_t i = 0; i < expected_types.size(); ++i) {
        assert(tokens[i].type == expected_types[i]);
    }

    assertToken(tokens, 1, TokenType::Identifier, "login_fail_review");
    assertToken(tokens, 3, TokenType::Identifier, "login");
    assertToken(tokens, 5, TokenType::Number, "100");
    assertToken(tokens, 7, TokenType::Identifier, "user.login_fail_5m");
    assertToken(tokens, 8, TokenType::Ge, ">=");
    assertToken(tokens, 9, TokenType::Number, "5");
    assertToken(tokens, 13, TokenType::String, "too_many_failed_login");

    assert(tokens[0].line == 1);
    assert(tokens[0].column == 1);
    assert(tokens[2].line == 2);
    assert(tokens[2].column == 1);
    assert(tokens[6].line == 4);
    assert(tokens[6].column == 1);
}

void test_logical_tokens() {
    const std::string input =
        "IF NOT (ip.in_topk == true OR cms.risk_behavior_count >= 100) "
        "AND device.distinct_account_10m != 0";

    RuleLexer lexer(input);
    const auto tokens = lexer.tokenize();

    assertToken(tokens, 0, TokenType::If, "IF");
    assertToken(tokens, 1, TokenType::Not, "NOT");
    assertToken(tokens, 2, TokenType::LParen, "(");
    assertToken(tokens, 3, TokenType::Identifier, "ip.in_topk");
    assertToken(tokens, 4, TokenType::Eq, "==");
    assertToken(tokens, 5, TokenType::True, "true");
    assertToken(tokens, 6, TokenType::Or, "OR");
    assertToken(tokens, 7, TokenType::Identifier, "cms.risk_behavior_count");
    assertToken(tokens, 8, TokenType::Ge, ">=");
    assertToken(tokens, 9, TokenType::Number, "100");
    assertToken(tokens, 10, TokenType::RParen, ")");
    assertToken(tokens, 11, TokenType::And, "AND");
    assertToken(tokens, 12, TokenType::Identifier, "device.distinct_account_10m");
    assertToken(tokens, 13, TokenType::Ne, "!=");
    assertToken(tokens, 14, TokenType::Number, "0");
    assert(tokens.back().type == TokenType::End);
}

void test_comparison_tokens() {
    const std::string input =
        "IF user.login_1m > 1 AND user.login_5m < 2 "
        "AND user.login_1h <= 3 AND ip.in_topk == false";

    RuleLexer lexer(input);
    const auto tokens = lexer.tokenize();

    assertToken(tokens, 2, TokenType::Gt, ">");
    assertToken(tokens, 6, TokenType::Lt, "<");
    assertToken(tokens, 10, TokenType::Le, "<=");
    assertToken(tokens, 14, TokenType::Eq, "==");
    assertToken(tokens, 15, TokenType::False, "false");
}
void test_comments_are_skipped() {
    const std::string input =
        "# default login rule\n"
        "RULE login_fail_review # inline comment\n"
        "SCENE all\n";

    RuleLexer lexer(input);
    const auto tokens = lexer.tokenize();

    assertToken(tokens, 0, TokenType::Rule, "RULE");
    assertToken(tokens, 1, TokenType::Identifier, "login_fail_review");
    assertToken(tokens, 2, TokenType::Scene, "SCENE");
    assertToken(tokens, 3, TokenType::Identifier, "all");
    assert(tokens[4].type == TokenType::End);
}

void test_invalid_character_throws() {
    bool thrown = false;

    try {
        RuleLexer lexer("IF @");
        lexer.tokenize();
    } catch (const std::runtime_error&) {
        thrown = true;
    }

    assert(thrown);
}

void test_unterminated_string_throws() {
    bool thrown = false;

    try {
        RuleLexer lexer("THEN REVIEW REASON \"too_many_failed_login");
        lexer.tokenize();
    } catch (const std::runtime_error&) {
        thrown = true;
    }

    assert(thrown);
}

int main() {
    test_single_rule_tokens();
    test_logical_tokens();
    test_comparison_tokens();
    test_comments_are_skipped();
    test_invalid_character_throws();
    test_unterminated_string_throws();

    std::cout << "test_rule_lexer passed" << std::endl;
    return 0;
}