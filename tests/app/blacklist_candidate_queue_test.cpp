#include "aegisflow/app/blacklist_candidate_queue.hpp"

#include "tests/support/test_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using aegisflow::test::require;
using aegisflow::app::BlacklistCandidateQueue;
using aegisflow::app::CandidateSubmitStatus;

aegisflow::risk::BlacklistMutation upsert(
    const aegisflow::risk::EntityType type,
    std::string id,
    const std::uint64_t expire_at_ms
) {
    auto mutation = aegisflow::risk::BlacklistMutation::upsert(
        type,
        std::move(id),
        "candidate-test",
        expire_at_ms
    );
    require(mutation.has_value(), "队列测试候选必须合法");
    return std::move(*mutation);
}

void capacityCountsQueuedAndReserved() {
    bool rejected_zero = false;
    try {
        BlacklistCandidateQueue invalid(0);
    } catch (const std::invalid_argument&) {
        rejected_zero = true;
    }
    require(rejected_zero, "零容量必须拒绝");

    BlacklistCandidateQueue queue(2);
    require(
        queue.trySubmit(upsert(aegisflow::risk::EntityType::Ip, "192.0.2.1", 100)) ==
                CandidateSubmitStatus::Accepted &&
            queue.trySubmit(upsert(aegisflow::risk::EntityType::Ip, "192.0.2.2", 200)) ==
                CandidateSubmitStatus::Accepted &&
            queue.trySubmit(upsert(aegisflow::risk::EntityType::Ip, "192.0.2.3", 300)) ==
                CandidateSubmitStatus::Full,
        "队列必须在固定容量上非阻塞拒绝"
    );

    const auto reserved = queue.reserveBatch(1);
    require(reserved.size() == 1, "必须只保留指定批量");
    require(
        queue.trySubmit(upsert(aegisflow::risk::EntityType::Ip, "192.0.2.4", 400)) ==
            CandidateSubmitStatus::Full,
        "reserved 仍必须占用队列容量"
    );
    auto stats = queue.stats();
    require(
        stats.queued == 1 && stats.reserved == 1 && stats.dropped == 2,
        "queued、reserved 与 dropped 计数不一致"
    );
    require(queue.takeDroppedSinceLastReport() == 2 &&
                queue.takeDroppedSinceLastReport() == 0,
            "queue-full 摘要只能报告自上次以来的增量");

    queue.acknowledgeReserved();
    require(
        queue.trySubmit(upsert(aegisflow::risk::EntityType::Ip, "192.0.2.4", 400)) ==
            CandidateSubmitStatus::Accepted,
        "确认 reserved 后必须归还容量"
    );
    require(queue.trySubmit(upsert(
                aegisflow::risk::EntityType::Ip, "192.0.2.5", 500)) ==
                CandidateSubmitStatus::Full &&
                queue.takeDroppedSinceLastReport() == 1,
            "新的 queue-full 应形成下一条增量摘要");
}

void batchDeduplicatesAndPermanentExpiryWins() {
    BlacklistCandidateQueue queue(6);
    require(
        queue.trySubmit(upsert(aegisflow::risk::EntityType::Ip, "192.0.2.8", 100)) ==
            CandidateSubmitStatus::Accepted,
        "首条候选必须接受"
    );
    (void)queue.trySubmit(upsert(aegisflow::risk::EntityType::Ip, "192.0.2.8", 200));
    (void)queue.trySubmit(upsert(aegisflow::risk::EntityType::Ip, "192.0.2.8", 0));
    (void)queue.trySubmit(upsert(aegisflow::risk::EntityType::Ip, "192.0.2.8", 500));
    (void)queue.trySubmit(upsert(aegisflow::risk::EntityType::User, "user-8", 300));

    const auto batch = queue.reserveBatch(6);
    require(batch.size() == 2, "批内必须按 (type,id) 去重");
    const auto* ip = &batch.front();
    if (ip->entityType() != aegisflow::risk::EntityType::Ip) {
        ip = &batch.back();
    }
    require(
        ip->entityType() == aegisflow::risk::EntityType::Ip &&
            ip->id() == "192.0.2.8" && ip->expireAtMs() == 0,
        "重复 UPSERT 必须保留更晚过期，0 比有限 TTL 更晚"
    );
    const auto stats = queue.stats();
    require(
        stats.queued == 0 && stats.reserved == 2,
        "去重后 reserved 容量必须以唯一候选计数"
    );
}

void failedBatchRetriesBeforeNewQueuedValues() {
    BlacklistCandidateQueue queue(4);
    (void)queue.trySubmit(upsert(aegisflow::risk::EntityType::Ip, "192.0.2.11", 100));
    (void)queue.trySubmit(upsert(aegisflow::risk::EntityType::Ip, "192.0.2.12", 100));
    (void)queue.trySubmit(upsert(aegisflow::risk::EntityType::Ip, "192.0.2.13", 100));

    const auto first = queue.reserveBatch(2);
    require(first.size() == 2, "首批数量错误");
    require(
        queue.trySubmit(upsert(aegisflow::risk::EntityType::Ip, "192.0.2.14", 100)) ==
            CandidateSubmitStatus::Accepted,
        "reserved 与 queued 总量未达上限时必须接受"
    );

    // 不 acknowledge 表示 Redis 事务失败。即使新上限更小也必须整批优先重试。
    const auto retry = queue.reserveBatch(1);
    require(retry == first, "失败批次必须保留原值并优先重试");

    queue.acknowledgeReserved();
    const auto next = queue.reserveBatch(8);
    require(
        next.size() == 2 && next[0].id() == "192.0.2.13" &&
            next[1].id() == "192.0.2.14",
        "成功确认后才能继续下一批并保持先入顺序"
    );
}

void closeRejectsNewValuesButKeepsAcceptedWork() {
    BlacklistCandidateQueue queue(2);
    (void)queue.trySubmit(upsert(aegisflow::risk::EntityType::Ip, "192.0.2.21", 100));
    queue.close();
    require(
        queue.trySubmit(upsert(aegisflow::risk::EntityType::Ip, "192.0.2.22", 100)) ==
                CandidateSubmitStatus::Closed &&
            queue.stats().closed && queue.stats().dropped == 0,
        "close 后必须拒绝新值，但不把其误计为队满丢弃"
    );
    require(
        queue.reserveBatch(2).size() == 1,
        "close 后已接受候选仍必须可排空"
    );

    const auto disable = aegisflow::risk::BlacklistMutation::disable(
        aegisflow::risk::EntityType::Ip,
        "192.0.2.23"
    );
    require(disable.has_value(), "disable 测试值必须合法");
    BlacklistCandidateQueue open_queue(1);
    require(
        open_queue.trySubmit(*disable) == CandidateSubmitStatus::Invalid &&
            open_queue.stats().dropped == 0,
        "自动候选队列只接受 UPSERT"
    );
}

void concurrentProducersCannotExceedCapacity() {
    constexpr std::size_t capacity = 64;
    constexpr std::size_t producer_count = 4;
    constexpr std::size_t each = 40;
    BlacklistCandidateQueue queue(capacity);
    std::vector<std::thread> producers;
    producers.reserve(producer_count);

    for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([producer, &queue]() {
            for (std::size_t index = 0; index < each; ++index) {
                const auto suffix = producer * each + index + 1;
                (void)queue.trySubmit(upsert(
                    aegisflow::risk::EntityType::User,
                    "parallel-user-" + std::to_string(suffix),
                    100 + suffix
                ));
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }

    const auto stats = queue.stats();
    require(
        stats.queued == capacity && stats.reserved == 0 &&
            stats.dropped == producer_count * each - capacity,
        "多产生者不得突破固定容量，丢弃数必须可对账"
    );
}

void shutdownDiscardCountsRemainingExactlyOnce() {
    BlacklistCandidateQueue queue(3);
    (void)queue.trySubmit(upsert(
        aegisflow::risk::EntityType::User, "shutdown-a", 100));
    (void)queue.trySubmit(upsert(
        aegisflow::risk::EntityType::User, "shutdown-b", 100));
    (void)queue.trySubmit(upsert(
        aegisflow::risk::EntityType::User, "shutdown-c", 100));
    require(queue.reserveBatch(1).size() == 1,
            "停机测试应保留 reserved");
    queue.close();
    require(queue.discardRemainingOnShutdown() == 3 &&
                queue.discardRemainingOnShutdown() == 0,
            "queued + reserved 只能在停机时计数一次");
    const auto stats = queue.stats();
    require(queue.empty() && stats.dropped_on_shutdown == 3 &&
                stats.dropped == 0,
            "停机丢弃不得混入运行期 queue-full 计数");
}

}  // namespace

int main() {
    return aegisflow::test::runModule(
        "blacklist_candidate_queue",
        {
            {"队列与 reserved 共用容量", capacityCountsQueuedAndReserved},
            {"批内去重与永久 TTL", batchDeduplicatesAndPermanentExpiryWins},
            {"失败批次优先重试", failedBatchRetriesBeforeNewQueuedValues},
            {"close 保留已接受值", closeRejectsNewValuesButKeepsAcceptedWork},
            {"多产生者容量不变式", concurrentProducersCannotExceedCapacity},
            {"停机剩余值只计一次", shutdownDiscardCountsRemainingExactlyOnce},
        }
    );
}
