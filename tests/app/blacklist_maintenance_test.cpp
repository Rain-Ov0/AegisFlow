#include "aegisflow/app/blacklist_maintenance.hpp"
#include "aegisflow/storage/redis_connection.hpp"
#include "../support/test_harness.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

using aegisflow::app::BlacklistMaintenanceConfig;
using aegisflow::risk::BlacklistEntry;
using aegisflow::risk::BlacklistMutation;
using aegisflow::risk::EntityType;
using aegisflow::storage::PendingBlacklistEntry;
using aegisflow::storage::StoreStatus;
using aegisflow::test::require;

constexpr std::uint64_t kNowMs = 10'000;

BlacklistMutation upsert(
    const EntityType type,
    std::string id,
    const std::uint64_t expiry = 60'000
) {
    auto value = BlacklistMutation::upsert(
        type, std::move(id), "test", expiry);
    require(value.has_value(), "测试候选必须合法");
    return std::move(*value);
}

aegisflow::storage::StoreResult makeStoreResult(
    const StoreStatus status,
    std::optional<std::uint64_t> revision = std::nullopt
) {
    aegisflow::storage::StoreResult result;
    result.status = status;
    result.revision = revision;
    return result;
}

aegisflow::storage::SnapshotReadResult makeSnapshotReadResult(
    const StoreStatus status,
    const std::uint64_t revision,
    std::vector<BlacklistEntry> entries = {}
) {
    aegisflow::storage::SnapshotReadResult result;
    result.status = status;
    result.revision = revision;
    result.entries = std::move(entries);
    return result;
}

PendingBlacklistEntry makePendingEntry(
    std::string stream_id,
    std::optional<BlacklistMutation> mutation,
    std::string error
) {
    PendingBlacklistEntry entry;
    entry.stream_id = std::move(stream_id);
    entry.mutation = std::move(mutation);
    entry.error = std::move(error);
    return entry;
}

aegisflow::storage::ResetReadResult makeResetReadResult(
    const StoreStatus status,
    const std::uint64_t requested = 0,
    const std::uint64_t completed = 0
) {
    aegisflow::storage::ResetReadResult result;
    result.status = status;
    result.state.requested = requested;
    result.state.completed = completed;
    return result;
}

struct FakeState {
    std::deque<StoreStatus> candidate_statuses;
    StoreStatus candidate_default_status = StoreStatus::Ok;
    std::vector<std::vector<std::string>> candidate_batches;
    std::uint64_t next_candidate_revision = 20;

    StoreStatus revision_status = StoreStatus::Ok;
    std::uint64_t revision = 5;
    std::deque<aegisflow::storage::SnapshotReadResult> snapshots;
    std::atomic<std::size_t> snapshot_calls{0};

    std::deque<aegisflow::storage::StoreResult> remove_results;
    std::atomic<std::size_t> remove_calls{0};
    std::vector<std::size_t> removed_batch_sizes;
    std::deque<StoreStatus> published_statuses;
    std::vector<std::uint64_t> published_revisions;

    std::vector<PendingBlacklistEntry> pending;
    std::deque<bool> mysql_results;
    std::atomic<std::size_t> mysql_calls{0};
    std::vector<std::vector<std::string>> deleted_batches;

    aegisflow::storage::ResetReadResult barrier =
        makeResetReadResult(StoreStatus::Ok);
    std::vector<std::uint64_t> completed_tokens;
    std::atomic<std::size_t> barrier_calls{0};

    std::mutex block_mutex;
    std::condition_variable block_cv;
    bool block_barrier = false;
    bool barrier_entered = false;
    bool release_barrier = false;
};

template <class T>
T popOr(std::deque<T>& scripted, T fallback) {
    if (scripted.empty()) {
        return fallback;
    }
    auto value = std::move(scripted.front());
    scripted.pop_front();
    return value;
}

class FakeBackend final
    : public aegisflow::app::IBlacklistMaintenanceBackend {
public:
    explicit FakeBackend(std::shared_ptr<FakeState> state)
        : state_(std::move(state)) {}

    aegisflow::storage::StoreResult applyCandidates(
        const aegisflow::base::ArrayView<const BlacklistMutation> mutations,
        aegisflow::storage::RedisDeadline
    ) override {
        std::vector<std::string> ids;
        for (const auto& mutation : mutations) {
            ids.push_back(mutation.id());
        }
        state_->candidate_batches.push_back(std::move(ids));
        const auto status = popOr(
            state_->candidate_statuses, state_->candidate_default_status);
        if (status != StoreStatus::Ok) {
            return makeStoreResult(status);
        }
        return makeStoreResult(
            StoreStatus::Ok, state_->next_candidate_revision++);
    }

    aegisflow::storage::StoreResult readRevision(
        aegisflow::storage::RedisDeadline
    ) override {
        return makeStoreResult(
            state_->revision_status,
            state_->revision_status == StoreStatus::Ok
                ? std::optional<std::uint64_t>{state_->revision}
                : std::nullopt);
    }

    aegisflow::storage::SnapshotReadResult loadStableSnapshot(
        std::size_t,
        aegisflow::storage::RedisDeadline
    ) override {
        ++state_->snapshot_calls;
        if (state_->snapshots.empty()) {
            return makeSnapshotReadResult(
                StoreStatus::Ok, state_->revision);
        }
        auto value = std::move(state_->snapshots.front());
        state_->snapshots.pop_front();
        return value;
    }

    aegisflow::storage::StoreResult removeExpiredFields(
        const aegisflow::base::ArrayView<const BlacklistEntry> entries,
        std::uint64_t,
        std::uint64_t,
        aegisflow::storage::RedisDeadline
    ) override {
        ++state_->remove_calls;
        state_->removed_batch_sizes.push_back(entries.size());
        return popOr(
            state_->remove_results,
            makeStoreResult(StoreStatus::Ok, state_->revision + 1));
    }

    StoreStatus setPublishedRevision(
        const std::uint64_t revision,
        aegisflow::storage::RedisDeadline
    ) override {
        state_->published_revisions.push_back(revision);
        return popOr(state_->published_statuses, StoreStatus::Ok);
    }

    aegisflow::storage::PendingReadResult readPending(
        std::size_t,
        aegisflow::storage::RedisDeadline
    ) override {
        aegisflow::storage::PendingReadResult result;
        result.status = StoreStatus::Ok;
        result.entries = state_->pending;
        return result;
    }

    StoreStatus deletePending(
        const aegisflow::base::ArrayView<const std::string> stream_ids,
        aegisflow::storage::RedisDeadline
    ) override {
        state_->deleted_batches.emplace_back(
            stream_ids.begin(), stream_ids.end());
        return StoreStatus::Ok;
    }

    bool applyMysqlMutations(
        aegisflow::base::ArrayView<const BlacklistMutation>,
        aegisflow::storage::MysqlDeadline
    ) override {
        ++state_->mysql_calls;
        return popOr(state_->mysql_results, true);
    }

    aegisflow::storage::ResetReadResult readResetBarrier(
        aegisflow::storage::RedisDeadline
    ) override {
        ++state_->barrier_calls;
        std::unique_lock lock(state_->block_mutex);
        if (state_->block_barrier) {
            state_->barrier_entered = true;
            state_->block_cv.notify_all();
            state_->block_cv.wait(lock, [this] {
                return state_->release_barrier;
            });
        }
        return state_->barrier;
    }

    StoreStatus completeResetBarrier(
        const std::uint64_t token,
        aegisflow::storage::RedisDeadline
    ) override {
        state_->completed_tokens.push_back(token);
        state_->barrier.state.completed = token;
        return StoreStatus::Ok;
    }

private:
    std::shared_ptr<FakeState> state_;
};

class FakeFactory final
    : public aegisflow::app::IBlacklistMaintenanceBackendFactory {
public:
    explicit FakeFactory(std::shared_ptr<FakeState> state)
        : state_(std::move(state)) {}

    std::unique_ptr<aegisflow::app::IBlacklistMaintenanceBackend>
    create() override {
        ++create_calls;
        return std::make_unique<FakeBackend>(state_);
    }

    std::atomic<std::size_t> create_calls{0};

private:
    std::shared_ptr<FakeState> state_;
};

BlacklistMaintenanceConfig config(
    const std::size_t candidate_batch_size = 2
) {
    BlacklistMaintenanceConfig result;
    result.maintenance_interval = std::chrono::milliseconds(20);
    result.maintenance_timeout = std::chrono::milliseconds(500);
    result.expire_cleanup_interval = std::chrono::hours(1);
    result.batch_size = 4;
    result.candidate_batch_size = candidate_batch_size;
    return result;
}

void run(
    aegisflow::app::BlacklistMaintenanceCore& core,
    const std::uint64_t now_ms = kNowMs,
    const aegisflow::timer::SteadyTime steady_now =
        aegisflow::timer::SteadyClock::now()
) {
    core.runRound(
        {},
        {},
        aegisflow::timer::SteadyClock::now() + std::chrono::seconds(1),
        now_ms,
        steady_now);
}

void candidateFailureKeepsReservedFirst() {
    auto state = std::make_shared<FakeState>();
    state->candidate_statuses = {
        StoreStatus::IoError, StoreStatus::Ok, StoreStatus::Ok};
    auto factory = std::make_shared<FakeFactory>(state);
    aegisflow::app::BlacklistCandidateQueue queue(4);
    require(queue.trySubmit(upsert(EntityType::User, "a")) ==
                aegisflow::app::CandidateSubmitStatus::Accepted,
            "a 应入队");
    require(queue.trySubmit(upsert(EntityType::User, "b")) ==
                aegisflow::app::CandidateSubmitStatus::Accepted,
            "b 应入队");
    require(queue.trySubmit(upsert(EntityType::User, "c")) ==
                aegisflow::app::CandidateSubmitStatus::Accepted,
            "c 应入队");
    aegisflow::risk::BlacklistManager manager;
    aegisflow::app::BlacklistMaintenanceCore core(
        config(), factory, queue, manager, [] { return 0; }, 5, false,
        aegisflow::timer::SteadyClock::now() + std::chrono::hours(1));

    run(core);
    const auto failed = queue.stats();
    require(failed.reserved == 2 && failed.queued == 1,
            "Redis 失败后 reserved batch 必须原样保留");
    run(core);
    run(core);
    require(queue.empty(), "重试成功后应排空候选");
    require(state->candidate_batches.size() == 3 &&
                state->candidate_batches[0] ==
                    std::vector<std::string>({"a", "b"}) &&
                state->candidate_batches[1] ==
                    std::vector<std::string>({"a", "b"}) &&
                state->candidate_batches[2] ==
                    std::vector<std::string>({"c"}),
            "下轮必须先重试原 reserved batch");
}

void revisionControlsStablePublication() {
    auto state = std::make_shared<FakeState>();
    auto factory = std::make_shared<FakeFactory>(state);
    aegisflow::app::BlacklistCandidateQueue queue(2);
    aegisflow::risk::BlacklistManager manager;
    aegisflow::app::BlacklistMaintenanceCore core(
        config(), factory, queue, manager, [] { return 0; }, 5, false,
        aegisflow::timer::SteadyClock::now() + std::chrono::hours(1));

    run(core);
    require(state->snapshot_calls == 0,
            "revision 不变时不得重复 HSCAN");

    state->revision = 6;
    state->snapshots.push_back(makeSnapshotReadResult(
        StoreStatus::Ok,
        6,
        {{EntityType::Ip, "192.0.2.6", 0}}));
    run(core);
    require(state->snapshot_calls == 1 &&
                manager.matches("", "192.0.2.6", "", kNowMs).ip_hit,
            "revision 变化后应发布稳定快照");
    require(core.state().last_published_revision == 6,
            "本地 revision 应追上已发布快照");

    state->revision = 7;
    state->snapshots.push_back(makeSnapshotReadResult(
        StoreStatus::WatchConflict, 0));
    run(core);
    require(core.state().last_published_revision == 6 &&
                manager.matches("", "192.0.2.6", "", kNowMs).ip_hit,
            "HSCAN 期间 revision 变化不得发布混合快照");
}

void publishedRevisionRetriesWithoutRescan() {
    auto state = std::make_shared<FakeState>();
    state->published_statuses = {StoreStatus::IoError, StoreStatus::Ok};
    auto factory = std::make_shared<FakeFactory>(state);
    aegisflow::app::BlacklistCandidateQueue queue(2);
    aegisflow::risk::BlacklistManager manager;
    aegisflow::app::BlacklistMaintenanceCore core(
        config(), factory, queue, manager, [] { return 0; }, 5, true,
        aegisflow::timer::SteadyClock::now() + std::chrono::hours(1));

    run(core);
    require(core.state().publication_dirty,
            "published SET 首次失败必须保留 dirty");
    run(core);
    require(!core.state().publication_dirty &&
                state->published_revisions ==
                    std::vector<std::uint64_t>({5, 5}) &&
                state->snapshot_calls == 0,
            "revision 不变时仍应重试本地已发布版本");
}

void mysqlCommitGatesXdelAndRecovers() {
    auto state = std::make_shared<FakeState>();
    state->pending = {makePendingEntry(
        "1-0",
        upsert(EntityType::Device, "device-1"),
        {})};
    state->mysql_results = {false, true};
    auto factory = std::make_shared<FakeFactory>(state);
    aegisflow::app::BlacklistCandidateQueue queue(2);
    aegisflow::risk::BlacklistManager manager;
    aegisflow::app::BlacklistMaintenanceCore core(
        config(), factory, queue, manager, [] { return 0; }, 5, false,
        aegisflow::timer::SteadyClock::now() + std::chrono::hours(1));

    run(core);
    require(state->mysql_calls == 1 && state->deleted_batches.empty(),
            "MySQL 失败时必须保留 Stream 记录");
    run(core);
    require(state->mysql_calls == 2 && state->deleted_batches.size() == 1 &&
                state->deleted_batches.front() ==
                    std::vector<std::string>({"1-0"}),
            "MySQL 恢复并 COMMIT 后才能 XDEL");
}

void invalidPendingIsLoggedAndDeleted() {
    auto state = std::make_shared<FakeState>();
    state->pending = {makePendingEntry(
        "2-0", std::nullopt, "missing operation")};
    auto factory = std::make_shared<FakeFactory>(state);
    aegisflow::app::BlacklistCandidateQueue queue(2);
    aegisflow::risk::BlacklistManager manager;
    aegisflow::app::BlacklistMaintenanceCore core(
        config(), factory, queue, manager, [] { return 0; }, 5, false,
        aegisflow::timer::SteadyClock::now() + std::chrono::hours(1));

    run(core);
    require(state->mysql_calls == 0 && state->deleted_batches.size() == 1 &&
                state->deleted_batches.front() ==
                    std::vector<std::string>({"2-0"}),
            "无法解析的 Stream 记录应跳过 MySQL 并删除");
}

void expiryConflictNeverPublishesFilteredCandidate() {
    auto state = std::make_shared<FakeState>();
    state->revision = 8;
    state->snapshots.push_back(makeSnapshotReadResult(
        StoreStatus::Ok,
        8,
        {{EntityType::User, "expired", 9'000}}));
    state->snapshots.push_back(makeSnapshotReadResult(
        StoreStatus::Ok,
        8,
        {{EntityType::User, "expired", 9'000}}));
    state->remove_results.push_back(
        makeStoreResult(StoreStatus::WatchConflict));
    state->remove_results.push_back(
        makeStoreResult(StoreStatus::Ok, 9));
    auto factory = std::make_shared<FakeFactory>(state);
    aegisflow::app::BlacklistCandidateQueue queue(2);
    aegisflow::risk::BlacklistManager manager;
    require(manager.publish({{EntityType::User, "old", 0}}, kNowMs),
            "应建立旧快照");
    const auto cleanup_now = aegisflow::timer::SteadyClock::now();
    aegisflow::app::BlacklistMaintenanceCore core(
        config(), factory, queue, manager, [] { return 0; }, 8, false,
        cleanup_now);

    run(core, kNowMs, cleanup_now);
    require(manager.matches("old", "", "", kNowMs).user_hit &&
                core.state().last_published_revision == 8 &&
                state->published_revisions.empty(),
            "field 已续期导致 WATCH 冲突时不得发布过滤快照");

    run(core, kNowMs, cleanup_now);
    require(!manager.matches("old", "", "", kNowMs).user_hit &&
                core.state().last_published_revision == 9 &&
                state->published_revisions ==
                    std::vector<std::uint64_t>({9}),
            "受 revision 保护的删除成功后才发布过滤快照");
}

void duplicateHscanFieldsAreCollapsedBeforeExpiryRemoval() {
    auto state = std::make_shared<FakeState>();
    state->revision = 12;
    state->snapshots.push_back(makeSnapshotReadResult(
        StoreStatus::Ok,
        12,
        {
            {EntityType::Ip, "192.0.2.12", 9'000},
            {EntityType::Ip, "192.0.2.12", 9'000},
        }));
    state->remove_results.push_back(
        makeStoreResult(StoreStatus::Ok, 13));
    auto factory = std::make_shared<FakeFactory>(state);
    aegisflow::app::BlacklistCandidateQueue queue(2);
    aegisflow::risk::BlacklistManager manager;
    const auto cleanup_now = aegisflow::timer::SteadyClock::now();
    aegisflow::app::BlacklistMaintenanceCore core(
        config(), factory, queue, manager, [] { return 0; }, 12, false,
        cleanup_now);

    run(core, kNowMs, cleanup_now);
    require(state->remove_calls == 1 &&
                state->removed_batch_sizes ==
                    std::vector<std::size_t>({1}) &&
                core.state().last_published_revision == 13,
            "HSCAN 重复 field 应在进入过期删除事务前折叠");
}

void barrierChecksBothInflightObservations() {
    auto state = std::make_shared<FakeState>();
    state->barrier.state.requested = 3;
    state->barrier.state.completed = 2;
    auto factory = std::make_shared<FakeFactory>(state);
    aegisflow::app::BlacklistCandidateQueue queue(2);
    require(queue.trySubmit(upsert(EntityType::Ip, "192.0.2.3")) ==
                aegisflow::app::CandidateSubmitStatus::Accepted,
            "barrier 候选应入队");
    aegisflow::risk::BlacklistManager manager;
    std::vector<std::uint64_t> inflight{1};
    std::size_t index = 0;
    aegisflow::app::BlacklistMaintenanceCore core(
        config(), factory, queue, manager,
        [&] {
            const auto position = std::min(index, inflight.size() - 1);
            ++index;
            return inflight[position];
        },
        5, false,
        aegisflow::timer::SteadyClock::now() + std::chrono::hours(1));

    run(core);
    require(state->completed_tokens.empty() &&
                state->candidate_batches.empty(),
            "首次 inflight 非零时不得排候选或 ack");

    inflight = {0, 1};
    index = 0;
    run(core);
    require(state->candidate_batches.size() == 1 &&
                state->completed_tokens.empty() && queue.empty(),
            "第二次 inflight 非零时仍不得 ack");

    inflight = {0, 0};
    index = 0;
    run(core);
    require(state->completed_tokens == std::vector<std::uint64_t>({3}),
            "两次 inflight 为零且候选空时才能 ack");
}

void barrierDrainsAllBatchesAndRetainsFailure() {
    auto state = std::make_shared<FakeState>();
    state->barrier.state.requested = 1;
    state->barrier.state.completed = 0;
    state->candidate_statuses = {
        StoreStatus::Ok, StoreStatus::IoError};
    state->candidate_default_status = StoreStatus::IoError;
    auto factory = std::make_shared<FakeFactory>(state);
    aegisflow::app::BlacklistCandidateQueue queue(4);
    for (const auto* id : {"a", "b", "c"}) {
        require(queue.trySubmit(upsert(EntityType::Device, id)) ==
                    aegisflow::app::CandidateSubmitStatus::Accepted,
                "barrier backlog 应入队");
    }
    aegisflow::risk::BlacklistManager manager;
    aegisflow::app::BlacklistMaintenanceCore core(
        config(1), factory, queue, manager, [] { return 0; }, 5, false,
        aegisflow::timer::SteadyClock::now() + std::chrono::hours(1));

    core.runRound(
        {},
        {},
        aegisflow::timer::SteadyClock::now() +
            std::chrono::milliseconds(15),
        kNowMs,
        aegisflow::timer::SteadyClock::now());
    auto retained = queue.stats();
    require(state->completed_tokens.empty() && retained.reserved == 1 &&
                retained.queued == 1,
            "超过一批且 reserved 失败时 barrier 不得提前 ack");
    state->candidate_batches.clear();
    state->candidate_statuses.clear();
    state->candidate_default_status = StoreStatus::Ok;
    run(core);
    require(queue.empty() &&
                state->completed_tokens == std::vector<std::uint64_t>({1}) &&
                state->candidate_batches ==
                    std::vector<std::vector<std::string>>(
                        {{"b"}, {"c"}}),
            "barrier 应先重试 reserved，再排空全部 backlog");
}

class FakeTimerScheduler final : public aegisflow::timer::ITimerScheduler {
public:
    aegisflow::timer::TimerScheduleResult scheduleAt(
        aegisflow::timer::SteadyTime,
        std::weak_ptr<aegisflow::timer::ITimerSink>,
        const aegisflow::timer::TimerEvent event
    ) noexcept override {
        std::lock_guard lock(mutex_);
        events_.push_back(event);
        const auto value = next_id_++;
        aegisflow::timer::TimerScheduleResult result;
        result.status = aegisflow::timer::TimerStatus::Ok;
        result.id.value = value;
        result.id.generation = 1;
        return result;
    }

    aegisflow::timer::TimerStatus cancel(
        aegisflow::timer::TimerId
    ) noexcept override {
        return aegisflow::timer::TimerStatus::Ok;
    }

    aegisflow::timer::TimerEvent event(const std::size_t index) const {
        std::lock_guard lock(mutex_);
        return events_.at(index);
    }

    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return events_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<aegisflow::timer::TimerEvent> events_;
    std::uint64_t next_id_ = 1;
};

void timerTicksNeverAccumulateWorkerTasks() {
    auto state = std::make_shared<FakeState>();
    state->block_barrier = true;
    auto factory = std::make_shared<FakeFactory>(state);
    aegisflow::app::BlacklistCandidateQueue queue(2);
    aegisflow::risk::BlacklistManager manager;
    aegisflow::runtime::BoundedWorkerPool business({1, 2});
    aegisflow::runtime::BoundedWorkerPool maintenance_pool({1, 2});
    FakeTimerScheduler scheduler;
    auto maintenance = aegisflow::app::BlacklistMaintenance::create(
        config(), factory, queue, business, maintenance_pool, scheduler,
        manager, 5, false);

    require(maintenance->start() && scheduler.size() == 1,
            "start 应只安排一个 tick");
    require(maintenance->tryPost(scheduler.event(0)),
            "首个 tick 应提交 maintenance task");
    {
        std::unique_lock lock(state->block_mutex);
        require(state->block_cv.wait_for(
                    lock, std::chrono::seconds(1),
                    [&] { return state->barrier_entered; }),
                "fake maintenance task 应进入阻塞点");
    }
    require(scheduler.size() == 2 &&
                maintenance->tryPost(scheduler.event(1)),
            "inflight 期间的下一 tick 应被合并");
    require(maintenance_pool.inflightCount() == 1,
            "合并 tick 不得在 worker pool 中堆积第二任务");

    {
        std::lock_guard lock(state->block_mutex);
        state->release_barrier = true;
    }
    state->block_cv.notify_all();
    const auto limit = std::chrono::steady_clock::now() +
                       std::chrono::seconds(1);
    while (maintenance->state().completed_rounds < 2 &&
           std::chrono::steady_clock::now() < limit) {
        std::this_thread::yield();
    }
    require(maintenance->state().completed_rounds == 2 &&
                state->barrier_calls == 2,
            "阻塞期间的 tick 应合并为释放后立即补交的唯一一轮");

    maintenance->stop();
    maintenance_pool.close();
    require(maintenance_pool.drainUntil(
                std::chrono::steady_clock::now() + std::chrono::seconds(1)) ==
                aegisflow::runtime::WorkerPoolStatus::Ok,
            "maintenance pool 应排空");
    require(maintenance_pool.join() ==
                aegisflow::runtime::WorkerPoolStatus::Ok,
            "maintenance pool 应 join");
    business.close();
    require(business.join() == aegisflow::runtime::WorkerPoolStatus::Ok,
            "business pool 应 join");
}

void deadlineAndStopExitBeforeIo() {
    auto state = std::make_shared<FakeState>();
    auto factory = std::make_shared<FakeFactory>(state);
    aegisflow::app::BlacklistCandidateQueue queue(2);
    aegisflow::risk::BlacklistManager manager;
    aegisflow::app::BlacklistMaintenanceCore core(
        config(), factory, queue, manager, [] { return 0; }, 5, false,
        aegisflow::timer::SteadyClock::now());

    core.runRound(
        {},
        {},
        aegisflow::timer::SteadyClock::now() - std::chrono::milliseconds(1),
        kNowMs, aegisflow::timer::SteadyClock::now());
    aegisflow::runtime::CancellationSource pool_stopped;
    require(pool_stopped.requestStop(), "pool stop source 应进入停止状态");
    core.runRound(
        pool_stopped.token(),
        {},
        aegisflow::timer::SteadyClock::now() + std::chrono::seconds(1),
        kNowMs, aegisflow::timer::SteadyClock::now());
    aegisflow::runtime::CancellationSource maintenance_stopped;
    require(maintenance_stopped.requestStop(),
            "maintenance stop source 应进入停止状态");
    core.runRound(
        {},
        maintenance_stopped.token(),
        aegisflow::timer::SteadyClock::now() + std::chrono::seconds(1),
        kNowMs, aegisflow::timer::SteadyClock::now());
    require(state->barrier_calls == 0 && state->snapshot_calls == 0 &&
                state->mysql_calls == 0,
            "deadline、pool stop 或 maintenance stop 必须在外部 I/O 前退出");
}

void stoppedMaintenanceUsesPoolTokenForFinalDrain() {
    auto state = std::make_shared<FakeState>();
    auto factory = std::make_shared<FakeFactory>(state);
    aegisflow::app::BlacklistCandidateQueue queue(4);
    for (const auto* id : {"final-a", "final-b", "final-c"}) {
        require(queue.trySubmit(upsert(EntityType::Device, id)) ==
                    aegisflow::app::CandidateSubmitStatus::Accepted,
                "最终排空候选应入队");
    }
    aegisflow::risk::BlacklistManager manager;
    aegisflow::runtime::BoundedWorkerPool business({1, 2});
    aegisflow::runtime::BoundedWorkerPool maintenance_pool({1, 2});
    FakeTimerScheduler scheduler;
    auto maintenance = aegisflow::app::BlacklistMaintenance::create(
        config(2), factory, queue, business, maintenance_pool, scheduler,
        manager, 5, false);

    maintenance->stop();
    require(maintenance->finalDrainUntil(
                std::chrono::steady_clock::now() +
                std::chrono::seconds(1)) &&
                queue.empty(),
            "stop 后最终任务必须使用 pool token 排空候选");
    require(state->candidate_batches ==
                std::vector<std::vector<std::string>>(
                    {{"final-a", "final-b"}, {"final-c"}}),
            "最终任务应在同一 worker 中循环排空所有批次");
    require(maintenance->finalDrainUntil(
                std::chrono::steady_clock::now() +
                std::chrono::seconds(1)) &&
                state->candidate_batches.size() == 2,
            "重复 final drain 不得重复处理已确认候选");

    maintenance_pool.close();
    require(maintenance_pool.drainUntil(
                std::chrono::steady_clock::now() +
                std::chrono::seconds(1)) ==
                aegisflow::runtime::WorkerPoolStatus::Ok &&
                maintenance_pool.join() ==
                    aegisflow::runtime::WorkerPoolStatus::Ok,
            "final drain 后 maintenance pool 应可排空并 join");
    business.close();
    require(business.join() == aegisflow::runtime::WorkerPoolStatus::Ok,
            "final drain 测试 business pool 应 join");
}

void finalRedisFailureRetainsThenDiscardsExactlyOnce() {
    auto state = std::make_shared<FakeState>();
    state->candidate_default_status = StoreStatus::IoError;
    auto factory = std::make_shared<FakeFactory>(state);
    aegisflow::app::BlacklistCandidateQueue queue(2);
    require(queue.trySubmit(upsert(EntityType::User, "drop-final-a")) ==
                aegisflow::app::CandidateSubmitStatus::Accepted &&
                queue.trySubmit(upsert(EntityType::User, "drop-final-b")) ==
                aegisflow::app::CandidateSubmitStatus::Accepted,
            "最终 Redis 失败测试候选应入队");
    aegisflow::risk::BlacklistManager manager;
    aegisflow::runtime::BoundedWorkerPool business({1, 1});
    aegisflow::runtime::BoundedWorkerPool maintenance_pool({1, 1});
    FakeTimerScheduler scheduler;
    auto maintenance = aegisflow::app::BlacklistMaintenance::create(
        config(), factory, queue, business, maintenance_pool, scheduler,
        manager, 5, false);
    maintenance->stop();
    require(!maintenance->finalDrainUntil(
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(15)),
            "Redis 持续失败到 deadline 时 final drain 必须失败");
    const auto retained = queue.stats();
    require(retained.queued + retained.reserved == 2,
            "未确认的 reserved/queued 必须保留到 worker 停止");

    maintenance_pool.close();
    (void)maintenance_pool.drainUntil(
        std::chrono::steady_clock::now() + std::chrono::seconds(1));
    require(maintenance_pool.join() ==
                aegisflow::runtime::WorkerPoolStatus::Ok,
            "Redis 失败后 maintenance pool 应 join");
    require(queue.discardRemainingOnShutdown() == 2 &&
                queue.discardRemainingOnShutdown() == 0 &&
                queue.stats().dropped_on_shutdown == 2,
            "deadline 后剩余候选必须精确且只计一次");
    business.close();
    require(business.join() == aegisflow::runtime::WorkerPoolStatus::Ok,
            "Redis 失败测试 business pool 应 join");
}

void finalDrainWaitsBehindBlockedNormalRound() {
    auto state = std::make_shared<FakeState>();
    state->block_barrier = true;
    auto factory = std::make_shared<FakeFactory>(state);
    aegisflow::app::BlacklistCandidateQueue queue(2);
    require(queue.trySubmit(upsert(EntityType::User, "fifo-final")) ==
                aegisflow::app::CandidateSubmitStatus::Accepted,
            "FIFO 测试候选应入队");
    aegisflow::risk::BlacklistManager manager;
    aegisflow::runtime::BoundedWorkerPool business({1, 2});
    aegisflow::runtime::BoundedWorkerPool maintenance_pool({1, 2});
    FakeTimerScheduler scheduler;
    auto maintenance = aegisflow::app::BlacklistMaintenance::create(
        config(), factory, queue, business, maintenance_pool, scheduler,
        manager, 5, false);
    require(maintenance->start() &&
                maintenance->tryPost(scheduler.event(0)),
            "普通 maintenance round 应入队");
    {
        std::unique_lock lock(state->block_mutex);
        require(state->block_cv.wait_for(
                    lock, std::chrono::seconds(1),
                    [&] { return state->barrier_entered; }),
                "普通 round 应阻塞在 fake Redis");
    }

    maintenance->stop();
    auto final_result = std::async(std::launch::async, [maintenance] {
        return maintenance->finalDrainUntil(
            std::chrono::steady_clock::now() + std::chrono::seconds(2));
    });
    const auto submit_limit = std::chrono::steady_clock::now() +
                              std::chrono::seconds(1);
    while (maintenance_pool.inflightCount() != 2 &&
           std::chrono::steady_clock::now() < submit_limit) {
        std::this_thread::yield();
    }
    require(maintenance_pool.inflightCount() == 2,
            "final task 应排在已阻塞的普通 round 之后");
    {
        std::lock_guard lock(state->block_mutex);
        state->release_barrier = true;
    }
    state->block_cv.notify_all();
    require(final_result.get() && queue.empty() &&
                state->candidate_batches ==
                    std::vector<std::vector<std::string>>({{"fifo-final"}}),
            "普通 round 观察 stop 后，FIFO final task 应使用 pool token 排空");

    maintenance_pool.close();
    require(maintenance_pool.drainUntil(
                std::chrono::steady_clock::now() +
                std::chrono::seconds(1)) ==
                aegisflow::runtime::WorkerPoolStatus::Ok &&
                maintenance_pool.join() ==
                    aegisflow::runtime::WorkerPoolStatus::Ok,
            "FIFO maintenance pool 应 join");
    business.close();
    require(business.join() == aegisflow::runtime::WorkerPoolStatus::Ok,
            "FIFO business pool 应 join");
}

struct WorkerGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
};

class GateTask final : public aegisflow::runtime::IWorkerTask {
public:
    explicit GateTask(std::shared_ptr<WorkerGate> gate)
        : gate_(std::move(gate)) {}

    void run(aegisflow::runtime::CancellationToken) const override {
        std::unique_lock lock(gate_->mutex);
        gate_->entered = true;
        gate_->changed.notify_all();
        gate_->changed.wait(lock, [this] { return gate_->released; });
    }

private:
    std::shared_ptr<WorkerGate> gate_;
};

void fullMaintenancePoolRejectsFinalSubmission() {
    auto state = std::make_shared<FakeState>();
    auto factory = std::make_shared<FakeFactory>(state);
    aegisflow::app::BlacklistCandidateQueue queue(2);
    aegisflow::risk::BlacklistManager manager;
    aegisflow::runtime::BoundedWorkerPool business({1, 1});
    aegisflow::runtime::BoundedWorkerPool maintenance_pool({1, 1});
    FakeTimerScheduler scheduler;
    auto maintenance = aegisflow::app::BlacklistMaintenance::create(
        config(), factory, queue, business, maintenance_pool, scheduler,
        manager, 5, false);
    auto gate = std::make_shared<WorkerGate>();
    require(maintenance_pool.trySubmit(std::make_unique<GateTask>(gate)) ==
                aegisflow::runtime::WorkerSubmitStatus::Accepted,
            "阻塞任务应进入 maintenance worker");
    {
        std::unique_lock lock(gate->mutex);
        require(gate->changed.wait_for(
                    lock, std::chrono::seconds(1),
                    [&] { return gate->entered; }),
                "gate task 应开始执行");
    }
    require(maintenance_pool.trySubmit(std::make_unique<GateTask>(gate)) ==
                aegisflow::runtime::WorkerSubmitStatus::Accepted,
            "第二任务应占满 maintenance 队列");
    maintenance->stop();
    require(!maintenance->finalDrainUntil(
                std::chrono::steady_clock::now() +
                std::chrono::seconds(1)),
            "maintenance pool 队列已满时 final submit 必须明确失败");

    {
        std::lock_guard lock(gate->mutex);
        gate->released = true;
    }
    gate->changed.notify_all();
    maintenance_pool.close();
    require(maintenance_pool.drainUntil(
                std::chrono::steady_clock::now() +
                std::chrono::seconds(1)) ==
                aegisflow::runtime::WorkerPoolStatus::Ok &&
                maintenance_pool.join() ==
                    aegisflow::runtime::WorkerPoolStatus::Ok,
            "队列满测试 maintenance pool 应 join");
    business.close();
    require(business.join() == aegisflow::runtime::WorkerPoolStatus::Ok,
            "队列满测试 business pool 应 join");
}

void productionFactoryCreatesLazyAdaptor() {
    aegisflow::storage::RedisConfig redis;
    redis.host.clear();
    auto factory =
        aegisflow::app::makeBlacklistMaintenanceBackendFactory(redis, {});
    require(factory != nullptr,
            "production maintenance factory 应可创建");
    auto backend = factory->create();
    require(backend != nullptr,
            "factory::create 只建立懒连接 adaptor");
    const auto result = backend->readRevision(
        std::chrono::steady_clock::now() - std::chrono::milliseconds(1));
    require(result.status == StoreStatus::IoError,
            "无效连接配置应由首个 worker 命令报错，不得在 create 阶段连接");
}

std::optional<aegisflow::storage::RedisConfig> integrationRedisConfig() {
    const char* host = std::getenv("AEGISFLOW_TEST_REDIS_HOST");
    const char* port_text = std::getenv("AEGISFLOW_TEST_REDIS_PORT");
    if (host == nullptr || port_text == nullptr) {
        return std::nullopt;
    }
    unsigned int port = 0;
    const std::string_view port_view(port_text);
    const auto parsed = std::from_chars(
        port_view.data(), port_view.data() + port_view.size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != port_view.data() +
                                             port_view.size() ||
        port == 0 || port > 65'535) {
        throw std::runtime_error("AEGISFLOW_TEST_REDIS_PORT invalid");
    }
    aegisflow::storage::RedisConfig config;
    config.host = host;
    config.port = static_cast<std::uint16_t>(port);
    if (const char* username =
            std::getenv("AEGISFLOW_TEST_REDIS_USERNAME")) {
        config.username = username;
    }
    if (const char* password =
            std::getenv("AEGISFLOW_TEST_REDIS_PASSWORD")) {
        config.password = password;
    }
    if (const char* database_text =
            std::getenv("AEGISFLOW_TEST_REDIS_DATABASE")) {
        unsigned int database = 0;
        const std::string_view view(database_text);
        const auto result = std::from_chars(
            view.data(), view.data() + view.size(), database);
        if (result.ec != std::errc{} || result.ptr != view.data() +
                                               view.size() ||
            database > 15) {
            throw std::runtime_error(
                "AEGISFLOW_TEST_REDIS_DATABASE invalid");
        }
        config.database = database;
    }
    config.key_prefix = "aegisflow:test:blacklist-maintenance:" +
                        std::to_string(static_cast<long long>(::getpid()));
    return config;
}

bool redisInteger(
    aegisflow::storage::RedisConnection& connection,
    std::vector<std::string> command,
    const std::int64_t expected
) {
    const auto result = connection.command(
        command,
        std::chrono::steady_clock::now() + std::chrono::seconds(2));
    return result.status == aegisflow::storage::RedisCommandStatus::Ok &&
           result.value.kind == aegisflow::storage::RedisValueKind::Integer &&
           result.value.integer == expected;
}

bool redisOk(
    aegisflow::storage::RedisConnection& connection,
    std::vector<std::string> command
) {
    const auto result = connection.command(
        command,
        std::chrono::steady_clock::now() + std::chrono::seconds(2));
    return result.status == aegisflow::storage::RedisCommandStatus::Ok &&
           result.value.kind == aegisflow::storage::RedisValueKind::Status &&
           result.value.text == "OK";
}

class ExactKeyCleanup final {
public:
    ExactKeyCleanup(
        aegisflow::storage::RedisConfig config,
        aegisflow::storage::RedisKeySet keys
    ) : config_(std::move(config)), keys_(std::move(keys)) {}

    ~ExactKeyCleanup() {
        auto connection = aegisflow::storage::RedisConnection::connect(
            config_, std::chrono::steady_clock::now() +
                         std::chrono::seconds(2));
        if (connection == nullptr) {
            return;
        }
        auto command = std::vector<std::string>{"DEL"};
        const auto keys = keys_.all();
        command.insert(command.end(), keys.begin(), keys.end());
        static_cast<void>(connection->command(
            command,
            std::chrono::steady_clock::now() + std::chrono::seconds(2)));
    }

private:
    aegisflow::storage::RedisConfig config_;
    aegisflow::storage::RedisKeySet keys_;
};

void productionAdaptorReconnectIntegration(
    const aegisflow::storage::RedisConfig& config
) {
    const auto keys = aegisflow::storage::RedisKeySet::fromPrefix(
        config.key_prefix);
    ExactKeyCleanup cleanup(config, keys);
    auto setup = aegisflow::storage::RedisConnection::connect(
        config,
        std::chrono::steady_clock::now() + std::chrono::seconds(2));
    require(setup != nullptr, "无法连接 Redis integration 实例");
    auto delete_all = std::vector<std::string>{"DEL"};
    const auto all_keys = keys.all();
    delete_all.insert(delete_all.end(), all_keys.begin(), all_keys.end());
    require(redisInteger(*setup, delete_all, 0),
            "integration prefix 必须初始为空");
    require(redisOk(*setup, {"SET", keys.cache_ready, "1"}) &&
                redisOk(*setup, {"SET", keys.revision, "0"}),
            "应初始化 ready/revision");

    auto factory = aegisflow::app::makeBlacklistMaintenanceBackendFactory(
        config, {});
    auto backend = factory->create();
    const std::vector mutations{
        upsert(EntityType::Ip, "2001:0db8:0:0:0:0:0:55", 0)};
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    const auto stored = backend->applyCandidates(mutations, deadline);
    require(stored.status == StoreStatus::Ok && stored.revision == 1,
            "production adaptor 应在首个 worker 命令懒连接并写入");

    const auto expired = backend->readRevision(
        std::chrono::steady_clock::now() - std::chrono::milliseconds(1));
    require(expired.status == StoreStatus::DeadlineExceeded,
            "过期 deadline 应使当前 Redis connection 失效");
    const auto reconnected = backend->readRevision(
        std::chrono::steady_clock::now() + std::chrono::seconds(2));
    require(reconnected.status == StoreStatus::Ok &&
                reconnected.revision == 1,
            "下一命令应重建 Redis connection 并读回同一 revision");
    const auto snapshot = backend->loadStableSnapshot(
        10, std::chrono::steady_clock::now() + std::chrono::seconds(2));
    require(snapshot.status == StoreStatus::Ok &&
                snapshot.entries.size() == 1 &&
                snapshot.entries.front().id == "2001:db8::55",
            "重连后 Store 应重建并读到标准 IP");

    auto verify = aegisflow::storage::RedisConnection::connect(
        config,
        std::chrono::steady_clock::now() + std::chrono::seconds(2));
    require(verify != nullptr && redisInteger(*verify, delete_all, 4),
            "integration 应删除 Hash/Stream/revision/ready 四个已创建 key");
    require(redisInteger(*verify, {"EXISTS", keys.users, keys.ips,
                                  keys.devices, keys.pending, keys.revision,
                                  keys.cache_ready, keys.published_revision,
                                  keys.reset_barrier}, 0),
            "integration 结束后八个精确 key 必须全部不存在");
}

}  // 命名空间

int main(const int argc, char** argv) {
    const bool integration_only =
        argc == 2 && std::string_view(argv[1]) == "--integration";
    if (argc > 2 || (argc == 2 && !integration_only)) {
        std::cerr << "usage: test_blacklist_maintenance [--integration]\n";
        return 2;
    }
    if (integration_only) {
        try {
            const auto redis = integrationRedisConfig();
            if (!redis.has_value()) {
                std::cout << "[SKIP] blacklist_maintenance_integration: set "
                             "AEGISFLOW_TEST_REDIS_HOST/PORT\n";
                return 0;
            }
            productionAdaptorReconnectIntegration(*redis);
            std::cout << "[PASS] blacklist_maintenance_integration: 1/1\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] blacklist_maintenance_integration: "
                      << error.what() << '\n';
            return 1;
        }
    }
    return aegisflow::test::runModule("blacklist maintenance", {
        {"candidate failure keeps reserved first",
         candidateFailureKeepsReservedFirst},
        {"revision controls stable publication",
         revisionControlsStablePublication},
        {"published revision retries without rescan",
         publishedRevisionRetriesWithoutRescan},
        {"mysql commit gates xdel and recovers",
         mysqlCommitGatesXdelAndRecovers},
        {"invalid pending is logged and deleted",
         invalidPendingIsLoggedAndDeleted},
        {"expiry conflict never publishes filtered candidate",
         expiryConflictNeverPublishesFilteredCandidate},
        {"duplicate hscan fields collapse before expiry removal",
         duplicateHscanFieldsAreCollapsedBeforeExpiryRemoval},
        {"barrier checks both inflight observations",
         barrierChecksBothInflightObservations},
        {"barrier drains all batches and retains failure",
         barrierDrainsAllBatchesAndRetainsFailure},
        {"timer ticks never accumulate worker tasks",
         timerTicksNeverAccumulateWorkerTasks},
        {"deadline and stop exit before io", deadlineAndStopExitBeforeIo},
        {"stopped maintenance uses pool token for final drain",
         stoppedMaintenanceUsesPoolTokenForFinalDrain},
        {"final redis failure retains then discards exactly once",
         finalRedisFailureRetainsThenDiscardsExactlyOnce},
        {"final drain waits behind blocked normal round",
         finalDrainWaitsBehindBlockedNormalRound},
        {"full maintenance pool rejects final submission",
         fullMaintenancePoolRejectsFinalSubmission},
        {"production factory creates lazy adaptor",
         productionFactoryCreatesLazyAdaptor},
    });
}
