#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>

#include "aegisflow/rule/rule_node.hpp"

namespace aegisflow::rule {

class RuleValidationError : public std::runtime_error {
public:
    RuleValidationError(size_t line, size_t column, const std::string& message)
        : std::runtime_error(
              "rule validation error at " + std::to_string(line) +
              ":" + std::to_string(column) + " " + message
          ) {}
};

class RuleValidator {
public:
    static void validate(const RuleSet& rule_set);
    static bool isSupportedFeature(const std::string& name);

private:
    static void validateRule(const Rule& rule, const RuleSet& rule_set);
    static void validateNode(const RuleNode& node, const RuleSet& rule_set);
    static void validateCondition(const Condition& condition);
};

} // namespace aegisflow::rule