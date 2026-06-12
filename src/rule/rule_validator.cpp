#include "aegisflow/rule/rule_validator.hpp"

#include <optional>
#include <string>
#include <unordered_map>

namespace aegisflow::rule {

namespace {

enum class FeatureKind {
    Number,
    Bool,
    String
};

const std::unordered_map<std::string, FeatureKind>& featureKinds() {
    static const std::unordered_map<std::string, FeatureKind> features = {
        {"user.login_1m", FeatureKind::Number},
        {"user.login_5m", FeatureKind::Number},
        {"user.login_1h", FeatureKind::Number},
        {"user.login_fail_5m", FeatureKind::Number},
        {"ip.distinct_user_10m", FeatureKind::Number},
        {"device.distinct_account_10m", FeatureKind::Number},
        {"cms.risk_behavior_count", FeatureKind::Number},
        {"ip.in_topk", FeatureKind::Bool},
        {"user.black_hit", FeatureKind::Bool},
        {"ip.black_hit", FeatureKind::Bool},
        {"device.black_hit", FeatureKind::Bool},
    };

    return features;
}

std::optional<FeatureKind> resolveFeatureKind(const std::string& name) {
    const auto& features = featureKinds();
    const auto it = features.find(name);
    if (it == features.end()) {
        return std::nullopt;
    }

    return it->second;
}

ValueType expectedValueType(FeatureKind kind) {
    switch (kind) {
    case FeatureKind::Number:
        return ValueType::Number;
    case FeatureKind::Bool:
        return ValueType::Bool;
    case FeatureKind::String:
        return ValueType::String;
    }

    return ValueType::String;
}

const char* valueTypeName(ValueType type) {
    switch (type) {
    case ValueType::Number:
        return "number";
    case ValueType::Bool:
        return "bool";
    case ValueType::String:
        return "string";
    }

    return "unknown";
}

bool isComparisonAllowed(FeatureKind kind, CompareOp op) {
    switch (kind) {
    case FeatureKind::Number:
        return true;

    case FeatureKind::Bool:
    case FeatureKind::String:
        return op == CompareOp::Eq || op == CompareOp::Ne;
    }

    return false;
}

std::string comparisonMessage(FeatureKind kind) {
    switch (kind) {
    case FeatureKind::Number:
        return "supports ==, !=, >, >=, <, and <=";

    case FeatureKind::Bool:
    case FeatureKind::String:
        return "only supports == and !=";
    }

    return "has unsupported comparison operators";
}

void failAt(const Condition& condition, const std::string& message) {
    throw RuleValidationError(condition.line, condition.column, message);
}

} // namespace

void RuleValidator::validate(const RuleSet& rule_set) {
    for (const auto& rule : rule_set.rules) {
        validateRule(rule, rule_set);
    }

    for (const auto& node : rule_set.nodes) {
        validateNode(node, rule_set);
    }
}

bool RuleValidator::isSupportedFeature(const std::string& name) {
    return resolveFeatureKind(name).has_value();
}

void RuleValidator::validateRule(const Rule& rule, const RuleSet& rule_set) {
    if (rule.root_node_id >= rule_set.nodes.size()) {
        throw RuleValidationError(
            1,
            1,
            "rule '" + rule.name + "' references missing root node " +
                std::to_string(rule.root_node_id)
        );
    }
}

void RuleValidator::validateNode(const RuleNode& node, const RuleSet& rule_set) {
    switch (node.type) {
    case NodeType::Condition:
        validateCondition(node.condition);
        return;

    case NodeType::And:
    case NodeType::Or:
        if (node.children.size() != 2) {
            throw RuleValidationError(
                1,
                1,
                "logic node " + std::to_string(node.id) + " must have 2 children"
            );
        }
        break;

    case NodeType::Not:
        if (node.children.size() != 1) {
            throw RuleValidationError(
                1,
                1,
                "NOT node " + std::to_string(node.id) + " must have 1 child"
            );
        }
        break;
    }

    for (uint32_t child : node.children) {
        if (child >= rule_set.nodes.size()) {
            throw RuleValidationError(
                1,
                1,
                "logic node " + std::to_string(node.id) +
                    " references missing child node " + std::to_string(child)
            );
        }
    }
}

void RuleValidator::validateCondition(const Condition& condition) {
    const auto kind = resolveFeatureKind(condition.feature_name);
    if (!kind.has_value()) {
        failAt(condition, "unknown feature '" + condition.feature_name + "'");
    }

    const ValueType expected_type = expectedValueType(*kind);
    if (condition.expected.type != expected_type) {
        failAt(
            condition,
            "feature '" + condition.feature_name + "' expects " +
                valueTypeName(expected_type) + " value but got " +
                valueTypeName(condition.expected.type)
        );
    }

    if (!isComparisonAllowed(*kind, condition.op)) {
        failAt(
            condition,
            "feature '" + condition.feature_name + "' " +
                comparisonMessage(*kind)
        );
    }
}

} // namespace aegisflow::rule