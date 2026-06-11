#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "aegisflow/feature/user_state.hpp"
#include "aegisflow/rule/rule_node.hpp"

namespace aegisflow::rule {

struct RuleHit {
    uint64_t rule_id = 0;
    std::string rule_name;
    int priority = 0;
    DecisionAction action = DecisionAction::Pass;
    std::string reason_code;
};

class FeatureResolver {
public: 
    static std::optional<Value> get(
        const aegisflow::feature::FeatureSnapshot& features,
        const std::string& name
    );
};

class RuleEngine {
public:
    explicit RuleEngine(std::shared_ptr<const RuleSet> rule_set);
    
    std::vector<RuleHit> evaluate(
        const aegisflow::feature::FeatureSnapshot& features,
        const std::string& scene
    ) const;

private:
    bool evalNode(
        uint32_t node_id,
        const aegisflow::feature::FeatureSnapshot& features
    ) const;

    bool evalCondition(
        const Condition& condition,
        const aegisflow::feature::FeatureSnapshot& features
    ) const;

    static bool compareValues(
        const Value& actual,
        CompareOp op,
        const Value& expected
    );

    static bool compareNumber(double left, CompareOp op, double right);
    static bool compareBool(bool left, CompareOp op, bool right);
    static bool compareString(
        const std::string& left, 
        CompareOp op, 
        const std::string& right
    );

private:
    std::shared_ptr<const RuleSet> rule_set_;
};

} // namespace aegisflow::rule