#include "aegisflow/app/blacklist_maintenance.hpp"

#include "aegisflow/log/logger.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace aegisflow::app {
namespace {

bool validConfig(const BlacklistMaintenanceConfig& config) noexcept {
    constexpr auto maximum_duration = std::chrono::hours(24);
    return config.maintenance_interval.count() > 0 &&
           config.maintenance_timeout.count() > 0 &&
           config.expire_cleanup_interval.count() > 0 &&
           config.maintenance_interval <= maximum_duration &&
           config.maintenance_timeout <= maximum_duration &&
           config.expire_cleanup_interval <= maximum_duration &&
           config.batch_size > 0 && config.candidate_batch_size > 0;
}

std::uint64_t systemNowMs() noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
}

}  // 命名空间

BlacklistMaintenanceCore::BlacklistMaintenanceCore(
    BlacklistMaintenanceConfig config,
    std::shared_ptr<IBlacklistMaintenanceBackendFactory> backend_factory,
    BlacklistCandidateQueue& candidate_queue,
    risk::BlacklistManager& manager,
    std::function<std::uint64_t()> business_inflight,
    const std::uint64_t initial_revision,
    const bool publication_dirty,
    const timer::SteadyTime first_expire_cleanup
) : config_(config),
    backend_factory_(std::move(backend_factory)),
    candidate_queue_(&candidate_queue),
    manager_(&manager),
    business_inflight_(std::move(business_inflight)),
    state_{},
    next_expire_cleanup_(first_expire_cleanup) {
    state_.last_published_revision = initial_revision;
    state_.publication_dirty = publication_dirty;
    if (!validConfig(config_) || backend_factory_ == nullptr ||
        !business_inflight_) {
        throw std::invalid_argument("黑名单维护核心配置无效");
    }
}

bool BlacklistMaintenanceCore::ensureBackend() noexcept {
    if (backend_ != nullptr) {
        return true;
    }
    try {
        backend_ = backend_factory_->create();
    } catch (...) {
        backend_.reset();
    }
    return backend_ != nullptr;
}

bool BlacklistMaintenanceCore::shouldStop(
    const runtime::CancellationToken pool_stop_token,
    const runtime::CancellationToken maintenance_stop_token,
    const timer::SteadyTime deadline
) const noexcept {
    return pool_stop_token.stopRequested() ||
           maintenance_stop_token.stopRequested() ||
           timer::SteadyClock::now() >= deadline;
}

bool BlacklistMaintenanceCore::flushOneCandidateBatch(
    const runtime::CancellationToken pool_stop_token,
    const runtime::CancellationToken maintenance_stop_token,
    const timer::SteadyTime deadline
) {
    if (shouldStop(pool_stop_token, maintenance_stop_token, deadline)) {
        return false;
    }
    auto batch = candidate_queue_->reserveBatch(config_.candidate_batch_size);
    if (batch.empty()) {
        return true;
    }

    const auto stored = backend_->applyCandidates(batch, deadline);
    if (stored.status != storage::StoreStatus::Ok ||
        !stored.revision.has_value()) {
        // reserved batch 仍由队列持有；失败时绝不回塞或确认，
        // 下一轮 reserveBatch 必然先返回这一批。
        return false;
    }
    candidate_queue_->acknowledgeReserved();
    return true;
}

bool BlacklistMaintenanceCore::drainCandidatesUntil(
    const runtime::CancellationToken pool_stop_token,
    const runtime::CancellationToken maintenance_stop_token,
    const timer::SteadyTime deadline
) noexcept {
    const auto dropped = candidate_queue_->takeDroppedSinceLastReport();
    if (dropped != 0) {
        AEGISFLOW_LOG_WARN(
            "blacklist candidate queue full; dropped=" +
            std::to_string(dropped));
    }
    if (!ensureBackend()) {
        return false;
    }
    try {
        while (!candidate_queue_->empty()) {
            if (shouldStop(
                    pool_stop_token, maintenance_stop_token, deadline)) {
                return false;
            }
            if (flushOneCandidateBatch(
                    pool_stop_token, maintenance_stop_token, deadline)) {
                continue;
            }
            if (shouldStop(
                    pool_stop_token, maintenance_stop_token, deadline)) {
                return false;
            }
            // reset barrier 与停机最终排空共用绝对 deadline。
            // 短退让避免 Redis 立即拒绝时在单 worker 中忙等，
            // 下次仍由 reserveBatch 优先返回原 reserved batch。
            const auto remaining = deadline - timer::SteadyClock::now();
            std::this_thread::sleep_for(std::min(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    remaining),
                std::chrono::milliseconds(10)));
        }
        return true;
    } catch (...) {
        // 未知异常发生在哪个协议阶段，不能继续复用可能
        // 停在 WATCH/MULTI 或 MySQL 事务中的后端对象。
        backend_.reset();
        return false;
    }
}

bool BlacklistMaintenanceCore::handleResetBarrier(
    const storage::ResetReadResult& barrier,
    const runtime::CancellationToken pool_stop_token,
    const runtime::CancellationToken maintenance_stop_token,
    const timer::SteadyTime deadline
) {
    if (barrier.state.requested <= barrier.state.completed) {
        return true;
    }

    // 第一次 inflight==0 证明已接受业务不再产生新候选。
    // 在这之后循环排空所有 queued/reserved，再做第二次
    // inflight 与队列联合检查；任一条件不成立都不确认 barrier。
    if (business_inflight_() != 0) {
        return false;
    }
    if (!drainCandidatesUntil(
            pool_stop_token, maintenance_stop_token, deadline)) {
        return false;
    }
    if (shouldStop(pool_stop_token, maintenance_stop_token, deadline) ||
        business_inflight_() != 0 ||
        !candidate_queue_->empty()) {
        return false;
    }
    return backend_->completeResetBarrier(
               barrier.state.requested, deadline) ==
           storage::StoreStatus::Ok;
}

void BlacklistMaintenanceCore::refreshSnapshotAndExpiry(
    const runtime::CancellationToken pool_stop_token,
    const runtime::CancellationToken maintenance_stop_token,
    const timer::SteadyTime deadline,
    const std::uint64_t now_ms,
    const timer::SteadyTime steady_now
) {
    if (shouldStop(pool_stop_token, maintenance_stop_token, deadline)) {
        return;
    }
    const auto revision = backend_->readRevision(deadline);
    if (revision.status != storage::StoreStatus::Ok ||
        !revision.revision.has_value()) {
        return;
    }

    const bool cleanup_due = steady_now >= next_expire_cleanup_;
    if (!cleanup_due &&
        *revision.revision == state_.last_published_revision) {
        return;
    }
    auto snapshot = backend_->loadStableSnapshot(config_.batch_size, deadline);
    if (snapshot.status != storage::StoreStatus::Ok ||
        shouldStop(pool_stop_token, maintenance_stop_token, deadline)) {
        // HSCAN 前后 revision 不同时 Store 返回 WatchConflict。
        // 本地继续使用旧不可变快照，不发布混合扫描结果。
        return;
    }

    // Redis HSCAN 允许在游标扫描期间重复返回 field。
    // 同一 (type,id) 值相同时折叠；值不同则说明扫描不能
    // 构成单一快照，整轮放弃，不把重复过期项交给删除事务。
    std::map<std::pair<risk::EntityType, std::string>, std::uint64_t> unique;
    for (const auto& entry : snapshot.entries) {
        const auto [position, inserted] = unique.emplace(
            std::pair{entry.type, entry.id}, entry.expire_at_ms);
        if (!inserted && position->second != entry.expire_at_ms) {
            return;
        }
    }

    std::vector<risk::BlacklistEntry> active_entries;
    std::vector<risk::BlacklistEntry> expired_entries;
    active_entries.reserve(unique.size());
    expired_entries.reserve(unique.size());
    for (const auto& [key, expiry] : unique) {
        risk::BlacklistEntry entry;
        entry.type = key.first;
        entry.id = key.second;
        entry.expire_at_ms = expiry;
        if (entry.expire_at_ms != 0 && entry.expire_at_ms <= now_ms) {
            expired_entries.push_back(std::move(entry));
        } else {
            active_entries.push_back(std::move(entry));
        }
    }

    std::uint64_t published_revision = snapshot.revision;
    if (cleanup_due && !expired_entries.empty()) {
        const auto removed = backend_->removeExpiredFields(
            expired_entries, snapshot.revision, now_ms, deadline);
        if (removed.status != storage::StoreStatus::Ok ||
            !removed.revision.has_value() ||
            shouldStop(
                pool_stop_token, maintenance_stop_token, deadline)) {
            // WATCH revision + HGET 过期值的冲突说明 field 可能已
            // 续期；此时连过滤后候选快照也不能发布。
            return;
        }
        published_revision = *removed.revision;
    }

    auto candidate = manager_->prepareCandidate(active_entries, now_ms);
    if (candidate == nullptr ||
        shouldStop(pool_stop_token, maintenance_stop_token, deadline) ||
        !manager_->publishCandidate(std::move(candidate))) {
        return;
    }
    state_.last_published_revision = published_revision;
    state_.publication_dirty = true;
    if (cleanup_due) {
        next_expire_cleanup_ = steady_now + config_.expire_cleanup_interval;
    }
}

void BlacklistMaintenanceCore::retryPublishedRevision(
    const runtime::CancellationToken pool_stop_token,
    const runtime::CancellationToken maintenance_stop_token,
    const timer::SteadyTime deadline
) {
    if (!state_.publication_dirty ||
        shouldStop(pool_stop_token, maintenance_stop_token, deadline)) {
        return;
    }
    if (backend_->setPublishedRevision(
            state_.last_published_revision, deadline) ==
        storage::StoreStatus::Ok) {
        state_.publication_dirty = false;
    }
}

void BlacklistMaintenanceCore::flushOnePendingBatch(
    const runtime::CancellationToken pool_stop_token,
    const runtime::CancellationToken maintenance_stop_token,
    const timer::SteadyTime deadline
) {
    if (shouldStop(pool_stop_token, maintenance_stop_token, deadline)) {
        return;
    }
    auto pending = backend_->readPending(config_.batch_size, deadline);
    if (pending.status != storage::StoreStatus::Ok ||
        pending.entries.empty()) {
        return;
    }

    std::vector<std::string> invalid_ids;
    std::vector<std::string> valid_ids;
    std::vector<risk::BlacklistMutation> mutations;
    invalid_ids.reserve(pending.entries.size());
    valid_ids.reserve(pending.entries.size());
    mutations.reserve(pending.entries.size());
    for (auto& entry : pending.entries) {
        if (!entry.mutation.has_value()) {
            AEGISFLOW_LOG_ERROR(
                "invalid blacklist pending entry id=" + entry.stream_id +
                " error=" + entry.error);
            invalid_ids.push_back(std::move(entry.stream_id));
            continue;
        }
        valid_ids.push_back(std::move(entry.stream_id));
        mutations.push_back(std::move(*entry.mutation));
    }

    // 坏记录没有可执行的业务语义，记 ERROR 后先删除，
    // 避免它永久卡住后续合法记录。
    if (!invalid_ids.empty()) {
        if (backend_->deletePending(invalid_ids, deadline) !=
            storage::StoreStatus::Ok) {
            return;
        }
    }
    if (mutations.empty() ||
        shouldStop(pool_stop_token, maintenance_stop_token, deadline)) {
        return;
    }

    // MySQL 事务按 Stream 原顺序提交。只有 COMMIT 已成功才
    // XDEL；提交后删除失败只会在下轮幂等重放，不会丢变更。
    if (!backend_->applyMysqlMutations(mutations, deadline) ||
        shouldStop(pool_stop_token, maintenance_stop_token, deadline)) {
        return;
    }
    static_cast<void>(backend_->deletePending(valid_ids, deadline));
}

void BlacklistMaintenanceCore::runRound(
    const runtime::CancellationToken pool_stop_token,
    const runtime::CancellationToken maintenance_stop_token,
    const timer::SteadyTime deadline,
    const std::uint64_t now_ms,
    const timer::SteadyTime steady_now
) noexcept {
    ++state_.completed_rounds;
    const auto dropped = candidate_queue_->takeDroppedSinceLastReport();
    if (dropped != 0) {
        AEGISFLOW_LOG_WARN(
            "blacklist candidate queue full; dropped=" +
            std::to_string(dropped));
    }
    if (!ensureBackend() ||
        shouldStop(pool_stop_token, maintenance_stop_token, deadline)) {
        return;
    }

    try {
        const auto barrier = backend_->readResetBarrier(deadline);
        if (barrier.status != storage::StoreStatus::Ok) {
            return;
        }
        const bool reset_pending =
            barrier.state.requested > barrier.state.completed;
        if (reset_pending) {
            if (!handleResetBarrier(
                    barrier,
                    pool_stop_token,
                    maintenance_stop_token,
                    deadline)) {
                return;
            }
        } else if (!flushOneCandidateBatch(
                       pool_stop_token,
                       maintenance_stop_token,
                       deadline)) {
            return;
        }

        refreshSnapshotAndExpiry(
            pool_stop_token,
            maintenance_stop_token,
            deadline,
            now_ms,
            steady_now);
        retryPublishedRevision(
            pool_stop_token, maintenance_stop_token, deadline);
        flushOnePendingBatch(
            pool_stop_token, maintenance_stop_token, deadline);
    } catch (...) {
        // 维护任务不得让异常逃逸并结束单线程 worker。
        // 未确认的 reserved batch 和 Stream 记录仍保持原状。
        backend_.reset();
    }
}

class BlacklistMaintenance::MaintenanceTask final
    : public runtime::IWorkerTask {
public:
    explicit MaintenanceTask(std::weak_ptr<BlacklistMaintenance> owner)
        : owner_(std::move(owner)) {}

    void run(const runtime::CancellationToken stop_token) const override {
        if (const auto owner = owner_.lock(); owner != nullptr) {
            owner->runRound(stop_token);
        }
    }

private:
    std::weak_ptr<BlacklistMaintenance> owner_;
};

class BlacklistMaintenance::FinalDrainTask final
    : public runtime::IWorkerTask {
public:
    FinalDrainTask(
        std::weak_ptr<BlacklistMaintenance> owner,
        const timer::SteadyTime deadline
    ) : owner_(std::move(owner)), deadline_(deadline) {}

    void run(
        const runtime::CancellationToken pool_stop_token
    ) const override {
        if (const auto owner = owner_.lock(); owner != nullptr) {
            owner->runFinalDrain(pool_stop_token, deadline_);
        }
    }

private:
    std::weak_ptr<BlacklistMaintenance> owner_;
    timer::SteadyTime deadline_;
};

std::shared_ptr<BlacklistMaintenance> BlacklistMaintenance::create(
    BlacklistMaintenanceConfig config,
    std::shared_ptr<IBlacklistMaintenanceBackendFactory> backend_factory,
    BlacklistCandidateQueue& candidate_queue,
    runtime::BoundedWorkerPool& business_pool,
    runtime::BoundedWorkerPool& maintenance_pool,
    timer::ITimerScheduler& timer_scheduler,
    risk::BlacklistManager& manager,
    const std::uint64_t initial_revision,
    const bool publication_dirty
) {
    if (!validConfig(config) || backend_factory == nullptr) {
        throw std::invalid_argument("黑名单维护配置无效");
    }
    return std::shared_ptr<BlacklistMaintenance>(new BlacklistMaintenance(
        config, std::move(backend_factory), candidate_queue, business_pool,
        maintenance_pool, timer_scheduler, manager, initial_revision,
        publication_dirty));
}

BlacklistMaintenance::BlacklistMaintenance(
    const BlacklistMaintenanceConfig config,
    std::shared_ptr<IBlacklistMaintenanceBackendFactory> backend_factory,
    BlacklistCandidateQueue& candidate_queue,
    runtime::BoundedWorkerPool& business_pool,
    runtime::BoundedWorkerPool& maintenance_pool,
    timer::ITimerScheduler& timer_scheduler,
    risk::BlacklistManager& manager,
    const std::uint64_t initial_revision,
    const bool publication_dirty
) : config_(config),
    maintenance_pool_(&maintenance_pool),
    timer_scheduler_(&timer_scheduler),
    core_(std::make_unique<BlacklistMaintenanceCore>(
        config,
        std::move(backend_factory),
        candidate_queue,
        manager,
        [&business_pool] { return business_pool.inflightCount(); },
        initial_revision,
        publication_dirty,
        timer::SteadyClock::now() + config.expire_cleanup_interval)),
    completed_state_(core_->state()) {}

BlacklistMaintenance::~BlacklistMaintenance() { stop(); }

bool BlacklistMaintenance::start() noexcept {
    std::lock_guard lock(mutex_);
    if (stopped_) {
        return false;
    }
    if (running_) {
        return true;
    }
    running_ = true;
    if (scheduleNextLocked()) {
        return true;
    }
    running_ = false;
    return false;
}

void BlacklistMaintenance::stop() noexcept {
    timer::TimerId timer_to_cancel;
    {
        std::lock_guard lock(mutex_);
        if (stopped_) {
            return;
        }
        stopped_ = true;
        running_ = false;
        active_sequence_ = 0;
        timer_to_cancel = active_timer_;
        active_timer_ = {};
    }
    static_cast<void>(maintenance_stop_source_.requestStop());
    if (timer_to_cancel.valid()) {
        static_cast<void>(timer_scheduler_->cancel(timer_to_cancel));
    }
}

bool BlacklistMaintenance::finalDrainUntil(
    const timer::SteadyTime deadline
) noexcept {
    std::unique_lock lock(mutex_);
    if (!stopped_ || timer::SteadyClock::now() >= deadline) {
        return false;
    }
    if (final_drain_finished_) {
        return final_drain_result_;
    }
    if (!final_drain_submitted_) {
        try {
            const auto submitted = maintenance_pool_->trySubmit(
                std::make_unique<FinalDrainTask>(weak_from_this(), deadline));
            if (submitted != runtime::WorkerSubmitStatus::Accepted) {
                final_drain_finished_ = true;
                final_drain_result_ = false;
                return false;
            }
            // maintenance pool 只有一个 worker，因此这个任务一定
            // 排在已接受的普通维护任务之后，形成 FIFO 停机栅栏。
            final_drain_submitted_ = true;
        } catch (...) {
            final_drain_finished_ = true;
            final_drain_result_ = false;
            return false;
        }
    }
    if (!final_drain_changed_.wait_until(lock, deadline, [this] {
            return final_drain_finished_;
        })) {
        return false;
    }
    return final_drain_result_;
}

bool BlacklistMaintenance::tryPost(const timer::TimerEvent event) noexcept {
    std::lock_guard lock(mutex_);
    if (!running_ ||
        event.kind != timer::TimerEventKind::BlacklistMaintenanceTick ||
        event.target_sequence == 0 ||
        event.target_sequence != active_sequence_) {
        return false;
    }
    active_timer_ = {};
    active_sequence_ = 0;
    const bool scheduled = scheduleNextLocked();
    if (!scheduled) {
        running_ = false;
    }
    if (round_inflight_) {
        // tick 只是“有工作可做”的信号。上一轮未结束时
        // 直接合并，不向有界 maintenance pool 堆积重复任务。
        round_pending_ = true;
        return true;
    }
    round_pending_ = false;
    const bool submitted = submitRoundLocked();
    if (!submitted) {
        round_pending_ = true;
    }
    return scheduled || submitted;
}

bool BlacklistMaintenance::scheduleNextLocked() noexcept {
    const auto sequence = next_sequence_++;
    if (sequence == 0) {
        return false;
    }
    timer::TimerEvent event;
    event.kind = timer::TimerEventKind::BlacklistMaintenanceTick;
    event.target_id = 1;
    event.target_sequence = sequence;
    const auto result = timer_scheduler_->scheduleAt(
        timer::SteadyClock::now() + config_.maintenance_interval,
        weak_from_this(),
        event);
    if (result.status != timer::TimerStatus::Ok) {
        return false;
    }
    active_timer_ = result.id;
    active_sequence_ = sequence;
    return true;
}

bool BlacklistMaintenance::submitRoundLocked() noexcept {
    try {
        const auto submitted = maintenance_pool_->trySubmit(
            std::make_unique<MaintenanceTask>(weak_from_this()));
        if (submitted != runtime::WorkerSubmitStatus::Accepted) {
            return false;
        }
        round_inflight_ = true;
        return true;
    } catch (...) {
        return false;
    }
}

void BlacklistMaintenance::runRound(
    const runtime::CancellationToken pool_stop_token
) noexcept {
    const auto maintenance_stop_token = maintenance_stop_source_.token();
    const auto steady_now = timer::SteadyClock::now();
    const auto deadline = steady_now + config_.maintenance_timeout;
    core_->runRound(
        pool_stop_token,
        maintenance_stop_token,
        deadline,
        systemNowMs(),
        steady_now);

    std::lock_guard lock(mutex_);
    completed_state_ = core_->state();
    round_inflight_ = false;
    if (running_ && round_pending_) {
        // 多个 inflight tick 只折叠成一个 pending bit。当前任务
        // 退出前立即补交一轮，候选不必再等一个完整周期。
        round_pending_ = false;
        if (!submitRoundLocked()) {
            round_pending_ = true;
        }
    }
}

void BlacklistMaintenance::runFinalDrain(
    const runtime::CancellationToken pool_stop_token,
    const timer::SteadyTime deadline
) noexcept {
    // stop() 已经停止周期维护令牌；最终排空必须只观察
    // pool token，否则任务一开始就会被已 stop 的令牌取消。
    const bool drained = core_->drainCandidatesUntil(
        pool_stop_token, {}, deadline);
    std::lock_guard lock(mutex_);
    completed_state_ = core_->state();
    final_drain_result_ = drained;
    final_drain_finished_ = true;
    final_drain_changed_.notify_all();
}

BlacklistMaintenanceState BlacklistMaintenance::state() const noexcept {
    std::lock_guard lock(mutex_);
    return completed_state_;
}

}  // 命名空间 aegisflow::app
