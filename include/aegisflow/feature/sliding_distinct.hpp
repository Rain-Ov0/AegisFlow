#pragma once

#include <cstddef>
#include <cstdint>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aegisflow::feature {

class SlidingDistinct {
private:
    struct StringHash {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(
            const std::string_view value
        ) const noexcept {
            return std::hash<std::string_view>{}(value);
        }

        [[nodiscard]] std::size_t operator()(
            const std::string& value
        ) const noexcept {
            return (*this)(std::string_view(value));
        }
    };

    struct StringEqual {
        using is_transparent = void;

        [[nodiscard]] bool operator()(
            const std::string_view left,
            const std::string_view right
        ) const noexcept {
            return left == right;
        }
    };

    struct Entry {
        std::uint64_t bucket = 0;
        std::string member;
    };

    struct EarlierBucket {
        bool operator()(const Entry& left, const Entry& right) const noexcept {
            return left.bucket > right.bucket;
        }
    };

public:
    SlidingDistinct(
        const std::uint64_t window_ms,
        const std::uint64_t bucket_ms,
        const std::size_t max_members
    ) {
        if (window_ms == 0 || bucket_ms == 0 || window_ms < bucket_ms ||
            window_ms % bucket_ms != 0 || max_members == 0) {
            throw std::invalid_argument("SlidingDistinct 配置无效");
        }
        window_bucket_num_ = window_ms / bucket_ms;
        bucket_ms_ = bucket_ms;
        max_members_ = max_members;
    }

    bool add(
        const std::string_view member,
        const std::uint64_t event_ts_ms,
        const std::uint64_t now_ms
    ) {
        expire(now_ms);
        const auto event_bucket = event_ts_ms / bucket_ms_;
        const auto now_bucket = now_ms / bucket_ms_;
        if (event_bucket > now_bucket ||
            event_bucket < minValidBucket(now_bucket)) {
            return false;
        }

        std::string member_key(member);
        auto found = last_seen_bucket_.find(member_key);
        if (found != last_seen_bucket_.end()) {
            if (event_bucket <= found->second) {
                return true;
            }
            expired_queue_.push({event_bucket, std::move(member_key)});
            found->second = event_bucket;
            return true;
        }
        if (last_seen_bucket_.size() >= max_members_) {
            // 成员上限是硬容量边界：饱和后拒绝新 key，已有 key 仍可刷新。
            return false;
        }

        const auto [inserted, created] = last_seen_bucket_.emplace(
            std::move(member_key), event_bucket);
        if (!created) {
            return false;
        }
        try {
            expired_queue_.push({event_bucket, inserted->first});
        } catch (...) {
            last_seen_bucket_.erase(inserted);
            throw;
        }
        return true;
    }

    std::uint32_t count(const std::uint64_t now_ms) {
        expire(now_ms);
        return static_cast<std::uint32_t>(last_seen_bucket_.size());
    }

    [[nodiscard]] std::size_t activeMemberCount() const noexcept {
        return last_seen_bucket_.size();
    }

private:
    [[nodiscard]] std::uint64_t minValidBucket(
        const std::uint64_t now_bucket
    ) const noexcept {
        return now_bucket < window_bucket_num_
                   ? 0
                   : now_bucket - window_bucket_num_ + 1;
    }

    void expire(const std::uint64_t now_ms) {
        const auto now_bucket = now_ms / bucket_ms_;
        if (clock_initialized_ && now_bucket < last_now_bucket_) {
            // 时钟回拨后清空旧时间线，选择短暂少计而不是把未来成员继续算入风险。
            last_seen_bucket_.clear();
            expired_queue_ = {};
        }
        clock_initialized_ = true;
        last_now_bucket_ = now_bucket;
        const auto minimum = minValidBucket(now_bucket);
        while (!expired_queue_.empty() &&
               expired_queue_.top().bucket < minimum) {
            const Entry& entry = expired_queue_.top();
            const auto found = last_seen_bucket_.find(entry.member);
            // 堆中允许同一成员有多个历史节点，只有最新桶匹配时才删除索引。
            if (found != last_seen_bucket_.end() &&
                found->second == entry.bucket) {
                last_seen_bucket_.erase(found);
            }
            expired_queue_.pop();
        }
    }

    std::uint64_t window_bucket_num_ = 0;
    std::uint64_t bucket_ms_ = 0;
    std::size_t max_members_ = 0;
    std::uint64_t last_now_bucket_ = 0;
    bool clock_initialized_ = false;
    std::unordered_map<std::string, std::uint64_t, StringHash, StringEqual>
        last_seen_bucket_;
    std::priority_queue<Entry, std::vector<Entry>, EarlierBucket>
        expired_queue_;
};

}  // 命名空间 aegisflow::feature
