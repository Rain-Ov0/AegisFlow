#include "aegisflow/feature/feature_store.hpp"

#include <functional>

namespace aegisflow::feature {

FeatureSnapshot FeatureStore::updateAndGet(
    const aegisflow::v1::Event& event,
    uint64_t now_ms
) {
    FeatureSnapshot snapshot;
    const std::string& user_id = event.user_id();
    snapshot.user_id = user_id;

    if (user_id.empty()) {
        return snapshot;
    }

    const bool event_time_valid = isEventTimeValid(event.timestamp_ms(), now_ms);
    
    {
        Shard& shard = shards_[shardIndex(user_id)];
        std::lock_guard<std::mutex> lock(shard.mutex);

        if (!event_time_valid) {
            auto it = shard.users.find(user_id);
            if (it != shard.users.end()) {
                snapshot = buildSnapshot(user_id, it->second, now_ms);
            }
        } else {
            auto [it, inserted] = shard.users.try_emplace(user_id);
            UserState& state = it->second;
            state.last_seen_ms = now_ms;

            if (event.type() == aegisflow::v1::LOGIN) {
                state.login_1m.add(event.timestamp_ms(), now_ms);
                state.login_5m.add(event.timestamp_ms(), now_ms);
                state.login_1h.add(event.timestamp_ms(), now_ms);

                if (event.result() == aegisflow::v1::FAIL) {
                    state.login_fail_5m.add(event.timestamp_ms(), now_ms);
                }
            }
            state.recent_actions.push({
                event.type(), 
                event.result(),
                event.timestamp_ms(),
            });
            snapshot = buildSnapshot(user_id, state, now_ms);
        }
    }

    if (!event_time_valid) {
        return snapshot;
    }

    updateIpDistinct(event, now_ms, snapshot);
    updateDeviceDistinct(event, now_ms, snapshot);
    updateTopK(event, snapshot);
    updateCms(event, snapshot);

    return snapshot;
}

void FeatureStore::updateIpDistinct(
    const aegisflow::v1::Event& event,
    uint64_t now_ms,
    FeatureSnapshot& out
) {
    if (event.ip().empty() || event.user_id().empty()) {
        return;
    }

    DistinctShard& shard = ip_shards_[shardIndex(event.ip())];
    std::lock_guard<std::mutex> lock(shard.mutex);

    auto [it, inserted] = shard.states.try_emplace(
        event.ip(),
        kDistinctWindowMs,
        kDistinctBucketMs,
        kDistinctMaxMembers
    );

    SlidingDistinct& distinct = it->second;
    distinct.add(event.user_id(), event.timestamp_ms(), now_ms);
    out.ip_distinct_user_10m = distinct.count(now_ms);
}

void FeatureStore::updateDeviceDistinct(
    const aegisflow::v1::Event& event,
    uint64_t now_ms,
    FeatureSnapshot& out
) {
    if (event.device_id().empty() || event.user_id().empty()) {
    return;
    }

    DistinctShard& shard = device_shards_[shardIndex(event.device_id())];
    std::lock_guard<std::mutex> lock(shard.mutex);

    auto [it, inserted] = shard.states.try_emplace(
        event.device_id(),
        kDistinctWindowMs,
        kDistinctBucketMs,
        kDistinctMaxMembers
    );

    SlidingDistinct& distinct = it->second;
    distinct.add(event.user_id(), event.timestamp_ms(), now_ms);
    out.device_distinct_account_10m = distinct.count(now_ms);
}

void FeatureStore::updateTopK(
    const aegisflow::v1::Event& event,
    FeatureSnapshot& out
) {
    if (event.ip().empty()) {
        return;
    }

    const auto result = ip_topk_.updateAndGet(event.ip());
    out.ip_topk_estimated_count = result.estimated_count;
    out.ip_in_topk = result.in_topk && 
        result.estimated_count >= kTopKMinEstimatedCount;
}

void FeatureStore::updateCms(
    const aegisflow::v1::Event& event,
    FeatureSnapshot& out
) {
    if (event.ip().empty()) {
        return ;
    }

    const std::string risk_key = buildRiskKey(event);
    out.cms_risk_behavior_count = risk_cms_.addAndEstimate(risk_key);
}

size_t FeatureStore::shardIndex(const std::string& user_id) const {
    return std::hash<std::string>()(user_id) % kShardNum;
}

bool FeatureStore::isEventTimeValid(uint64_t event_ts_ms, uint64_t now_ms) {
    if (event_ts_ms > now_ms) {
        return false;
    }
    return now_ms - event_ts_ms < kMaxWindows;
}

FeatureSnapshot FeatureStore::buildSnapshot(
    const std::string& user_id,
    UserState& state,
    uint64_t now_ms
) {
    FeatureSnapshot snapshot;
    snapshot.user_id = user_id;
    snapshot.user_login_1m = state.login_1m.sum(now_ms);
    snapshot.user_login_5m = state.login_5m.sum(now_ms);
    snapshot.user_login_1h = state.login_1h.sum(now_ms);
    snapshot.user_login_fail_5m = state.login_fail_5m.sum(now_ms);
    snapshot.recent_actions = state.recent_actions.list();
    return snapshot;
}

std::string FeatureStore::buildRiskKey(
    const aegisflow::v1::Event& event
) {
    std::string type = aegisflow::v1::EventType_Name(event.type());
    std::string result = aegisflow::v1::EventResult_Name(event.result());

    std::string key;
    key.reserve(event.ip().size() + event.scene().size() + type.size() + result.size() + 3);

    key.append(event.ip());
    key.push_back('|');
    key.append(event.scene());
    key.push_back('|'); 
    key.append(type);
    key.push_back('|');
    key.append(result);

    return key;
}

} // namespace aegisflow::feature