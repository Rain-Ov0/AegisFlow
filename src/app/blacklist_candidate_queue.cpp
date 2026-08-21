#include "aegisflow/app/blacklist_candidate_queue.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace aegisflow::app {

BlacklistCandidateQueue::BlacklistCandidateQueue(const std::size_t capacity)
    : capacity_(capacity) {
    if (capacity_ == 0) {
        throw std::invalid_argument("黑名单候选队列容量必须大于 0");
    }
    reserved_batch_.reserve(capacity_);
}

CandidateSubmitStatus BlacklistCandidateQueue::trySubmit(
    aegisflow::risk::BlacklistMutation candidate
) {
    if (!isCandidate(candidate)) {
        return CandidateSubmitStatus::Invalid;
    }

    std::lock_guard lock(mutex_);
    if (closed_) {
        return CandidateSubmitStatus::Closed;
    }

    // 容量同时包括排队项和 maintenance 已取走但未确认的唯一批次。
    // 这保证 Redis 持续失败时，业务线程也不会突破固定内存上限。
    if (queued_.size() + reserved_batch_.size() >= capacity_) {
        if (dropped_count_ != std::numeric_limits<std::uint64_t>::max()) {
            ++dropped_count_;
        }
        return CandidateSubmitStatus::Full;
    }

    queued_.push_back(std::move(candidate));
    return CandidateSubmitStatus::Accepted;
}

std::vector<aegisflow::risk::BlacklistMutation>
BlacklistCandidateQueue::reserveBatch(const std::size_t max_batch_size) {
    std::lock_guard lock(mutex_);
    if (!reserved_batch_.empty()) {
        // 上一批未确认时不从 queued 中取新值，保持重试顺序。
        return reserved_batch_;
    }
    if (max_batch_size == 0) {
        return {};
    }

    const std::size_t take_count =
        std::min(max_batch_size, queued_.size());
    for (std::size_t index = 0; index < take_count; ++index) {
        auto mutation = std::move(queued_.front());
        queued_.pop_front();
        mergeIntoReserved(reserved_batch_, std::move(mutation));
    }
    return reserved_batch_;
}

void BlacklistCandidateQueue::acknowledgeReserved() noexcept {
    std::lock_guard lock(mutex_);
    reserved_batch_.clear();
}

void BlacklistCandidateQueue::close() noexcept {
    std::lock_guard lock(mutex_);
    closed_ = true;
}

std::uint64_t
BlacklistCandidateQueue::discardRemainingOnShutdown() noexcept {
    std::lock_guard lock(mutex_);
    const auto remaining = queued_.size() + reserved_batch_.size();
    queued_.clear();
    reserved_batch_.clear();
    const auto available = std::numeric_limits<std::uint64_t>::max() -
                           dropped_on_shutdown_count_;
    const auto added = std::min<std::uint64_t>(remaining, available);
    dropped_on_shutdown_count_ += added;
    return added;
}

std::uint64_t
BlacklistCandidateQueue::takeDroppedSinceLastReport() noexcept {
    std::lock_guard lock(mutex_);
    const auto delta = dropped_count_ - reported_dropped_count_;
    reported_dropped_count_ = dropped_count_;
    return delta;
}

BlacklistCandidateQueueStats BlacklistCandidateQueue::stats() const noexcept {
    std::lock_guard lock(mutex_);
    BlacklistCandidateQueueStats result;
    result.queued = queued_.size();
    result.reserved = reserved_batch_.size();
    result.dropped = dropped_count_;
    result.dropped_on_shutdown = dropped_on_shutdown_count_;
    result.closed = closed_;
    return result;
}

bool BlacklistCandidateQueue::empty() const noexcept {
    std::lock_guard lock(mutex_);
    return queued_.empty() && reserved_batch_.empty();
}

bool BlacklistCandidateQueue::isCandidate(
    const aegisflow::risk::BlacklistMutation& mutation
) noexcept {
    return mutation.operation() ==
               aegisflow::risk::BlacklistMutationOperation::Upsert &&
           mutation.entityType().has_value();
}

bool BlacklistCandidateQueue::expiresLater(
    const std::uint64_t candidate_expire_at_ms,
    const std::uint64_t current_expire_at_ms
) noexcept {
    // expire_at_ms == 0 表示永不过期，在去重比较中比任意有限 TTL 更晚。
    return candidate_expire_at_ms == 0 ||
           (current_expire_at_ms != 0 &&
            candidate_expire_at_ms > current_expire_at_ms);
}

void BlacklistCandidateQueue::mergeIntoReserved(
    std::vector<aegisflow::risk::BlacklistMutation>& batch,
    aegisflow::risk::BlacklistMutation mutation
) {
    const auto existing = std::find_if(
        batch.begin(),
        batch.end(),
        [&mutation](const aegisflow::risk::BlacklistMutation& item) {
            return item.entityType() == mutation.entityType() &&
                   item.id() == mutation.id();
        }
    );
    if (existing == batch.end()) {
        batch.push_back(std::move(mutation));
        return;
    }
    if (expiresLater(mutation.expireAtMs(), existing->expireAtMs())) {
        *existing = std::move(mutation);
    }
}

}  // 命名空间 aegisflow::app
