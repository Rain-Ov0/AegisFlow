#pragma once

#include "aegisflow/risk/blacklist_mutation.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace aegisflow::app {

enum class CandidateSubmitStatus : std::uint8_t {
    Accepted,
    Full,
    Closed,
    Invalid,
};

struct BlacklistCandidateQueueStats {
    std::size_t queued = 0;
    std::size_t reserved = 0;
    std::uint64_t dropped = 0;
    std::uint64_t dropped_on_shutdown = 0;
    bool closed = false;
};

class BlacklistCandidateQueue final {
public:
    explicit BlacklistCandidateQueue(std::size_t capacity);

    BlacklistCandidateQueue(const BlacklistCandidateQueue&) = delete;
    BlacklistCandidateQueue& operator=(const BlacklistCandidateQueue&) =
        delete;

    [[nodiscard]] CandidateSubmitStatus trySubmit(
        aegisflow::risk::BlacklistMutation candidate
    );

    // maintenance 只有一个消费者。取出的批次仍由队列内部持有，
    // 因此 Redis 失败时无需回塞；下次会先返回同一 reserved batch。
    [[nodiscard]] std::vector<aegisflow::risk::BlacklistMutation>
    reserveBatch(std::size_t max_batch_size);

    // 只有外部持久化事务成功后才能确认，确认会归还这批容量。
    void acknowledgeReserved() noexcept;
    void close() noexcept;
    // maintenance worker 已停止后由 Handler 调用一次；
    // 返回本次确定丢弃的 queued + reserved 数量。
    [[nodiscard]] std::uint64_t discardRemainingOnShutdown() noexcept;
    // 单 maintenance 消费者按 tick 取走自上次摘要后的增量。
    [[nodiscard]] std::uint64_t takeDroppedSinceLastReport() noexcept;

    [[nodiscard]] BlacklistCandidateQueueStats stats() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    [[nodiscard]] static bool isCandidate(
        const aegisflow::risk::BlacklistMutation& mutation
    ) noexcept;
    [[nodiscard]] static bool expiresLater(
        std::uint64_t candidate_expire_at_ms,
        std::uint64_t current_expire_at_ms
    ) noexcept;
    static void mergeIntoReserved(
        std::vector<aegisflow::risk::BlacklistMutation>& batch,
        aegisflow::risk::BlacklistMutation mutation
    );

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<aegisflow::risk::BlacklistMutation> queued_;
    std::vector<aegisflow::risk::BlacklistMutation> reserved_batch_;
    std::uint64_t dropped_count_ = 0;
    std::uint64_t reported_dropped_count_ = 0;
    std::uint64_t dropped_on_shutdown_count_ = 0;
    bool closed_ = false;
};

}  // 命名空间 aegisflow::app
