#pragma once

#include "aegisflow/domain/login.hpp"
#include "aegisflow/risk/blacklist_mutation.hpp"

#include <vector>

namespace aegisflow::risk {

// 领域计算一次性交回风险决策和由该决策派生的候选变更。
// 值对象不引用 Protobuf、Redis 或队列，因此协议边界可以先完成
// 领域计算，再显式提交副作用。
struct RiskEvaluation {
    aegisflow::domain::LoginDecision decision;
    std::vector<BlacklistMutation> candidates;
};

}  // 命名空间 aegisflow::risk
