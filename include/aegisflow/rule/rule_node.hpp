#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aegisflow::rule {

enum class NodeType {
    Condition,
    And,
    Or,
    Not,
};

enum class CompareOp {
    Eq,
    Ne,
    Gt,
    Ge,
    Lt,
    Le
};

enum class ValueType {
    Number,
    Bool,
    String
};

struct Value {
    ValueType type = ValueType::Number;
    double number_value = 0;
    bool bool_value = false;
    std::string string_value;
};

struct Condition {
    std::string feature_name;
    CompareOp op = CompareOp::Eq;
    Value expected;
};

struct RuleNode {
    uint32_t id = 0;
    NodeType type = NodeType::Condition;
    Condition condition;
    std::vector<uint32_t> children;
};

enum class DecisionAction {
    Pass = 0,
    Review = 1,
    Reject = 2
};

struct Rule {
    uint64_t id = 0;
    std::string name;
    std::string scene;
    int priority = 0;
    DecisionAction action = DecisionAction::Pass;
    std::string reason_code;
    uint32_t root_node_id = 0;
};

struct RuleSet {
    std::vector<RuleNode> nodes;
    std::vector<Rule> rules;
};

} // namespace aegisflow::rule