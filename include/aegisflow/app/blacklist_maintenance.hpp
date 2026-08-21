#pragma once

#include "aegisflow/app/blacklist_candidate_queue.hpp"
#include "aegisflow/base/array_view.hpp"
#include "aegisflow/risk/blacklist_manager.hpp"
#include "aegisflow/runtime/bounded_worker_pool.hpp"
#include "aegisflow/runtime/cancellation.hpp"
#include "aegisflow/storage/blacklist_redis_store.hpp"
#include "aegisflow/storage/mysql_dao.hpp"
#include "aegisflow/timer/timer_core.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace aegisflow::app {

struct BlacklistMaintenanceConfig {
    std::chrono::milliseconds maintenance_interval{1'000};
    std::chrono::milliseconds maintenance_timeout{3'000};
    std::chrono::milliseconds expire_cleanup_interval{60'000};
    std::size_t batch_size = 100;
    std::size_t candidate_batch_size = 256;

    bool operator==(const BlacklistMaintenanceConfig& other) const {
        return maintenance_interval == other.maintenance_interval &&
               maintenance_timeout == other.maintenance_timeout &&
               expire_cleanup_interval == other.expire_cleanup_interval &&
               batch_size == other.batch_size &&
               candidate_batch_size == other.candidate_batch_size;
    }
};

// 这个 seam 只包含一轮维护实际需要的命令。生产实现拥有一条 Redis
// connection 和一个 MysqlDao；测试 fake 则能确定性制造事务冲突与断线。
class IBlacklistMaintenanceBackend {
public:
    virtual ~IBlacklistMaintenanceBackend() = default;

    [[nodiscard]] virtual storage::StoreResult applyCandidates(
        base::ArrayView<const risk::BlacklistMutation> mutations,
        storage::RedisDeadline deadline
    ) = 0;
    [[nodiscard]] virtual storage::StoreResult readRevision(
        storage::RedisDeadline deadline
    ) = 0;
    [[nodiscard]] virtual storage::SnapshotReadResult loadStableSnapshot(
        std::size_t scan_count,
        storage::RedisDeadline deadline
    ) = 0;
    [[nodiscard]] virtual storage::StoreResult removeExpiredFields(
        base::ArrayView<const risk::BlacklistEntry> entries,
        std::uint64_t expected_revision,
        std::uint64_t now_ms,
        storage::RedisDeadline deadline
    ) = 0;
    [[nodiscard]] virtual storage::StoreStatus setPublishedRevision(
        std::uint64_t revision,
        storage::RedisDeadline deadline
    ) = 0;
    [[nodiscard]] virtual storage::PendingReadResult readPending(
        std::size_t count,
        storage::RedisDeadline deadline
    ) = 0;
    [[nodiscard]] virtual storage::StoreStatus deletePending(
        base::ArrayView<const std::string> stream_ids,
        storage::RedisDeadline deadline
    ) = 0;
    [[nodiscard]] virtual bool applyMysqlMutations(
        base::ArrayView<const risk::BlacklistMutation> mutations,
        storage::MysqlDeadline deadline
    ) = 0;
    [[nodiscard]] virtual storage::ResetReadResult readResetBarrier(
        storage::RedisDeadline deadline
    ) = 0;
    [[nodiscard]] virtual storage::StoreStatus completeResetBarrier(
        std::uint64_t token,
        storage::RedisDeadline deadline
    ) = 0;
};

class IBlacklistMaintenanceBackendFactory {
public:
    virtual ~IBlacklistMaintenanceBackendFactory() = default;
    [[nodiscard]] virtual std::unique_ptr<IBlacklistMaintenanceBackend>
    create() = 0;
};

// 工厂对象只保存配置；真正的 Redis/MySQL connection 均由 worker 中首次
// 调用后端命令时创建，且断线后只在同一 worker 的下一命令重新建立。
[[nodiscard]] std::shared_ptr<IBlacklistMaintenanceBackendFactory>
makeBlacklistMaintenanceBackendFactory(
    storage::RedisConfig redis_config,
    storage::MysqlConfig mysql_config
);

struct BlacklistMaintenanceState {
    std::uint64_t last_published_revision = 0;
    bool publication_dirty = false;
    std::uint64_t completed_rounds = 0;
};

// 单线程状态机。只允许 maintenance worker 调用 runRound；类本身不加锁，
// 从结构上保证 Redis/MySQL connection 和本地 revision 没有并发访问者。
class BlacklistMaintenanceCore final {
public:
    BlacklistMaintenanceCore(
        BlacklistMaintenanceConfig config,
        std::shared_ptr<IBlacklistMaintenanceBackendFactory> backend_factory,
        BlacklistCandidateQueue& candidate_queue,
        risk::BlacklistManager& manager,
        std::function<std::uint64_t()> business_inflight,
        std::uint64_t initial_revision,
        bool publication_dirty,
        timer::SteadyTime first_expire_cleanup
    );

    BlacklistMaintenanceCore(const BlacklistMaintenanceCore&) = delete;
    BlacklistMaintenanceCore& operator=(const BlacklistMaintenanceCore&) =
        delete;

    void runRound(
        runtime::CancellationToken pool_stop_token,
        runtime::CancellationToken maintenance_stop_token,
        timer::SteadyTime deadline,
        std::uint64_t now_ms,
        timer::SteadyTime steady_now
    ) noexcept;

    // Handler 停机时关闭 candidate queue 后调用；这里只排空本地候选，
    // 已进入 pending Stream 的记录由本轮或下次启动继续幂等回写。
    [[nodiscard]] bool drainCandidatesUntil(
        runtime::CancellationToken pool_stop_token,
        runtime::CancellationToken maintenance_stop_token,
        timer::SteadyTime deadline
    ) noexcept;

    [[nodiscard]] BlacklistMaintenanceState state() const noexcept {
        return state_;
    }

private:
    [[nodiscard]] bool ensureBackend() noexcept;
    [[nodiscard]] bool shouldStop(
        runtime::CancellationToken pool_stop_token,
        runtime::CancellationToken maintenance_stop_token,
        timer::SteadyTime deadline
    ) const noexcept;
    [[nodiscard]] bool flushOneCandidateBatch(
        runtime::CancellationToken pool_stop_token,
        runtime::CancellationToken maintenance_stop_token,
        timer::SteadyTime deadline
    );
    [[nodiscard]] bool handleResetBarrier(
        const storage::ResetReadResult& barrier,
        runtime::CancellationToken pool_stop_token,
        runtime::CancellationToken maintenance_stop_token,
        timer::SteadyTime deadline
    );
    void refreshSnapshotAndExpiry(
        runtime::CancellationToken pool_stop_token,
        runtime::CancellationToken maintenance_stop_token,
        timer::SteadyTime deadline,
        std::uint64_t now_ms,
        timer::SteadyTime steady_now
    );
    void retryPublishedRevision(
        runtime::CancellationToken pool_stop_token,
        runtime::CancellationToken maintenance_stop_token,
        timer::SteadyTime deadline
    );
    void flushOnePendingBatch(
        runtime::CancellationToken pool_stop_token,
        runtime::CancellationToken maintenance_stop_token,
        timer::SteadyTime deadline
    );

    BlacklistMaintenanceConfig config_;
    std::shared_ptr<IBlacklistMaintenanceBackendFactory> backend_factory_;
    std::unique_ptr<IBlacklistMaintenanceBackend> backend_;
    BlacklistCandidateQueue* candidate_queue_;
    risk::BlacklistManager* manager_;
    std::function<std::uint64_t()> business_inflight_;
    BlacklistMaintenanceState state_;
    timer::SteadyTime next_expire_cleanup_;
};

class BlacklistMaintenance final
    : public timer::ITimerSink,
      public std::enable_shared_from_this<BlacklistMaintenance> {
public:
    [[nodiscard]] static std::shared_ptr<BlacklistMaintenance> create(
        BlacklistMaintenanceConfig config,
        std::shared_ptr<IBlacklistMaintenanceBackendFactory> backend_factory,
        BlacklistCandidateQueue& candidate_queue,
        runtime::BoundedWorkerPool& business_pool,
        runtime::BoundedWorkerPool& maintenance_pool,
        timer::ITimerScheduler& timer_scheduler,
        risk::BlacklistManager& manager,
        std::uint64_t initial_revision,
        bool publication_dirty
    );

    ~BlacklistMaintenance() override;
    BlacklistMaintenance(const BlacklistMaintenance&) = delete;
    BlacklistMaintenance& operator=(const BlacklistMaintenance&) = delete;

    [[nodiscard]] bool start() noexcept;
    void stop() noexcept;
    [[nodiscard]] bool finalDrainUntil(timer::SteadyTime deadline) noexcept;
    [[nodiscard]] bool tryPost(timer::TimerEvent event) noexcept override;
    [[nodiscard]] BlacklistMaintenanceState state() const noexcept;

private:
    class MaintenanceTask;
    class FinalDrainTask;
    BlacklistMaintenance(
        BlacklistMaintenanceConfig config,
        std::shared_ptr<IBlacklistMaintenanceBackendFactory> backend_factory,
        BlacklistCandidateQueue& candidate_queue,
        runtime::BoundedWorkerPool& business_pool,
        runtime::BoundedWorkerPool& maintenance_pool,
        timer::ITimerScheduler& timer_scheduler,
        risk::BlacklistManager& manager,
        std::uint64_t initial_revision,
        bool publication_dirty
    );

    [[nodiscard]] bool scheduleNextLocked() noexcept;
    [[nodiscard]] bool submitRoundLocked() noexcept;
    void runRound(runtime::CancellationToken pool_stop_token) noexcept;
    void runFinalDrain(
        runtime::CancellationToken pool_stop_token,
        timer::SteadyTime deadline
    ) noexcept;

    BlacklistMaintenanceConfig config_;
    runtime::BoundedWorkerPool* maintenance_pool_;
    timer::ITimerScheduler* timer_scheduler_;
    std::unique_ptr<BlacklistMaintenanceCore> core_;
    mutable std::mutex mutex_;
    timer::TimerId active_timer_;
    std::uint64_t active_sequence_ = 0;
    std::uint64_t next_sequence_ = 1;
    bool running_ = false;
    bool stopped_ = false;
    bool round_inflight_ = false;
    bool round_pending_ = false;
    runtime::CancellationSource maintenance_stop_source_;
    BlacklistMaintenanceState completed_state_;
    std::condition_variable final_drain_changed_;
    bool final_drain_submitted_ = false;
    bool final_drain_finished_ = false;
    bool final_drain_result_ = false;
};

}  // 命名空间 aegisflow::app
