#include "aegisflow/rule/rule_engine.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace aegisflow::rule {

namespace {

Value numberValue(double value) {
    Value result;
    result.type = ValueType::Number;
    result.number_value = value;
    return result;
}

Value boolValue(bool value) {
    Value result;
    result.type = ValueType::Bool;
    result.bool_value = value;
    return result;
}

} // namespace

std::optional<Value> FeatureResolver::get(
    const aegisflow::feature::FeatureSnapshot& features,
    const std::string& name
) {
    if (name == "user.login_1m") {
        return numberValue(static_cast<double>(features.user_login_1m));
    }

    if (name == "user.login_5m") {
        return numberValue(static_cast<double>(features.user_login_5m));
    }

    if (name == "user.login_1h") {
        return numberValue(static_cast<double>(features.user_login_1h));
    }

    if (name == "user.login_fail_5m") {
        return numberValue(static_cast<double>(features.user_login_fail_5m));
    }

    if (name == "ip.distinct_user_10m") {
        return numberValue(static_cast<double>(features.ip_distinct_user_10m));
    }

    if (name == "device.distinct_account_10m") {
        return numberValue(
            static_cast<double>(features.device_distinct_account_10m)
        );
    }

    if (name == "cms.risk_behavior_count") {
        return numberValue(
            static_cast<double>(features.cms_risk_behavior_count)
        );
    }

    if (name == "ip.in_topk") {
        return boolValue(features.ip_in_topk);
    }

    if (name == "user.black_hit") {
        return boolValue(features.user_black_hit);
    }

    if (name == "ip.black_hit") {
        return boolValue(features.ip_black_hit);
    }

    if (name == "device.black_hit") {
        return boolValue(features.device_black_hit);
    }

    return std::nullopt;
}

RuleEngine::RuleEngine(std::shared_ptr<const RuleSet> rule_set)
    : rule_set_(std::move(rule_set)) {
    if (!rule_set_) {
        throw std::runtime_error("rule_set must not be null");
    }
}

std::vector<RuleHit> RuleEngine::evaluate(
    const aegisflow::feature::FeatureSnapshot& features,
    const std::string& scene
) const {
    std::vector<RuleHit> hits;

    for (const auto& rule : rule_set_->rules) {
        if (rule.scene != "all" && rule.scene != scene) {
            continue;
        }

        if (evalNode(rule.root_node_id, features)) {
            hits.push_back({
                rule.id,
                rule.name,
                rule.priority,
                rule.action,
                rule.reason_code
            });
        }
    }

    std::sort(hits.begin(), hits.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.priority != rhs.priority) {
            return lhs.priority > rhs.priority;
        }
        return lhs.rule_id < rhs.rule_id;
    });

    return hits;
}

bool RuleEngine::evalNode(
    uint32_t node_id,
    const aegisflow::feature::FeatureSnapshot& features
) const {
    if (node_id >= rule_set_->nodes.size()) {
        return false;
    }

    const RuleNode& node = rule_set_->nodes[node_id];
    switch (node.type) {
    case NodeType::Condition:
        return evalCondition(node.condition, features);

    case NodeType::And:
        for (uint32_t child : node.children) {
            if (!evalNode(child, features)) {
                return false;
            }
        }
        return true;

    case NodeType::Or:
        for (uint32_t child : node.children) {
            if (evalNode(child, features)) {
                return true;
            }
        }
        return false;

    case NodeType::Not:
        if (node.children.empty()) {
            return false;
        }
        return !evalNode(node.children.front(), features);
    }

    return false;
}

bool RuleEngine::evalCondition(
    const Condition& condition,
    const aegisflow::feature::FeatureSnapshot& features
) const {
    const auto actual = FeatureResolver::get(features, condition.feature_name);
    if (!actual.has_value()) {
        return false;
    }

    return compareValues(*actual, condition.op, condition.expected);
}

bool RuleEngine::compareValues(
    const Value& actual,
    CompareOp op,
    const Value& expected
) {
    if (actual.type != expected.type) {
        return false;
    }

    switch (actual.type) {
    case ValueType::Number:
        return compareNumber(actual.number_value, op, expected.number_value);

    case ValueType::Bool:
        return compareBool(actual.bool_value, op, expected.bool_value);

    case ValueType::String:
        return compareString(actual.string_value, op, expected.string_value);
    }

    return false;
}

bool RuleEngine::compareNumber(double left, CompareOp op, double right) {
    switch (op) {
    case CompareOp::Eq:
        return left == right;
    case CompareOp::Ne:
        return left != right;
    case CompareOp::Gt:
        return left > right;
    case CompareOp::Ge:
        return left >= right;
    case CompareOp::Lt:
        return left < right;
    case CompareOp::Le:
        return left <= right;
    }

    return false;
}

bool RuleEngine::compareBool(bool left, CompareOp op, bool right) {
    switch (op) {
    case CompareOp::Eq:
        return left == right;
    case CompareOp::Ne:
        return left != right;
    case CompareOp::Gt:
    case CompareOp::Ge:
    case CompareOp::Lt:
    case CompareOp::Le:
        return false;
    }

    return false;
}

bool RuleEngine::compareString(
    const std::string& left,
    CompareOp op,
    const std::string& right
) {
    switch (op) {
    case CompareOp::Eq:
        return left == right;
    case CompareOp::Ne:
        return left != right;
    case CompareOp::Gt:
    case CompareOp::Ge:
    case CompareOp::Lt:
    case CompareOp::Le:
        return false;
    }

    return false;
}

} // namespace aegisflow::rule