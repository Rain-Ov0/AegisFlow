#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "aegisflow/rule/rule_node.hpp"
#include "aegisflow/rule/token.hpp"

namespace aegisflow::rule {

class RuleParseError : public std::runtime_error {
public:
    RuleParseError(size_t line, size_t column, const std::string& message)
        : std::runtime_error(
              "rule parse error at " + std::to_string(line) +
              ":" + std::to_string(column) + " " + message
          ) {}
};

class RuleParser {
public:
    explicit RuleParser(std::vector<Token> tokens);

    RuleSet parseRuleSet();

private:
    Rule parseRule();

    uint32_t parseExpr();
    uint32_t parseCondition();
    CompareOp parseCompareOp();
    Value parseValue();
    DecisionAction parseAction();

    Token consume(TokenType type, const std::string& message);
    bool match(TokenType type);
    bool check(TokenType type) const;
    bool isAtEnd() const;

    const Token& peek() const;
    const Token& previous() const;

    [[noreturn]] void fail(
        const Token& token,
        const std::string& message
    ) const;

    uint32_t makeConditionNode(const Condition& condition);

    static std::string conditionKey(const Condition& condition);
    static std::string opToString(CompareOp op);
    static std::string valueToString(const Value& value);

private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;

    RuleSet rule_set_;
    std::unordered_map<std::string, uint32_t> condition_cache_;
};

} // namespace aegisflow::rule