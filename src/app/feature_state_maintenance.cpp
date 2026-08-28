#include "aegisflow/app/feature_state_maintenance.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace aegisflow::app {

namespace {

std::uint64_t nowMs() noexcept {
    const auto elapsed = std::chrono::duration_cast<
        std::chrono::milliseconds
    >(std::chrono::system_clock::now().time_since_epoch()).count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
}

}  // 命名空间

class FeatureStateMaintenance::CleanupTask final
    : public runtime::IWorkerTask {
public:
    explicit CleanupTask(std::weak_ptr<FeatureStateMaintenance> owner)
        : owner_(std::move(owner)) {}

    void run(const runtime::CancellationToken stop_token) const override {
        if (const auto owner = owner_.lock(); owner != nullptr) {
            owner->runRound(stop_token);
        }
    }

private:
    std::weak_ptr<FeatureStateMaintenance> owner_;
};

std::shared_ptr<FeatureStateMaintenance> FeatureStateMaintenance::create(
    FeatureStateMaintenanceConfig config,
    runtime::BoundedWorkerPool& maintenance_pool,
    timer::ITimerScheduler& timer_scheduler,
    feature::LoginFeatureStore& feature_store
) {
    if (config.cleanup_interval.count() <= 0) {
        throw std::invalid_argument("状态回收维护配置无效");
    }
    return std::shared_ptr<FeatureStateMaintenance>(
        new FeatureStateMaintenance(
            config,
            maintenance_pool,
            timer_scheduler,
            feature_store
        )
    );
}

FeatureStateMaintenance::FeatureStateMaintenance(
    const FeatureStateMaintenanceConfig config,
    runtime::BoundedWorkerPool& maintenance_pool,
    timer::ITimerScheduler& timer_scheduler,
    feature::LoginFeatureStore& feature_store
) : config_(config),
    maintenance_pool_(&maintenance_pool),
    timer_scheduler_(&timer_scheduler),
    feature_store_(&feature_store) {}

FeatureStateMaintenance::~FeatureStateMaintenance() {
    stop();
}

bool FeatureStateMaintenance::start() noexcept {
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

void FeatureStateMaintenance::stop() noexcept {
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
    if (timer_to_cancel.valid()) {
        (void)timer_scheduler_->cancel(timer_to_cancel);
    }
}

bool FeatureStateMaintenance::tryPost(const timer::TimerEvent event) noexcept {
    std::lock_guard lock(mutex_);
    if (!running_ || event.kind != timer::TimerEventKind::CleanupTick ||
        event.target_sequence == 0 ||
        event.target_sequence != active_sequence_) {
        return false;
    }

    active_timer_ = {};
    active_sequence_ = 0;
    const bool scheduled = scheduleNextLocked();
    if (round_inflight_) {
        // 定时 tick 只表示“需要一轮”，已有回收在途时直接合并。
        return true;
    }
    const bool submitted = submitRoundLocked();
    return scheduled || submitted;
}

bool FeatureStateMaintenance::scheduleNextLocked() noexcept {
    const std::uint64_t sequence = next_sequence_++;
    if (sequence == 0) {
        return false;
    }
    timer::TimerEvent event;
    event.kind = timer::TimerEventKind::CleanupTick;
    event.target_id = 1;
    event.target_sequence = sequence;
    const auto result = timer_scheduler_->scheduleAt(
        timer::SteadyClock::now() + config_.cleanup_interval,
        weak_from_this(),
        event
    );
    if (result.status != timer::TimerStatus::Ok) {
        return false;
    }
    active_timer_ = result.id;
    active_sequence_ = sequence;
    return true;
}

bool FeatureStateMaintenance::submitRoundLocked() noexcept {
    try {
        const auto status = maintenance_pool_->trySubmit(
            std::make_unique<CleanupTask>(weak_from_this())
        );
        if (status != runtime::WorkerSubmitStatus::Accepted) {
            return false;
        }
        round_inflight_ = true;
        return true;
    } catch (...) {
        return false;
    }
}

void FeatureStateMaintenance::runRound(
    const runtime::CancellationToken stop_token
) noexcept {
    {
        std::lock_guard lock(mutex_);
        if (stopped_ || stop_token.stopRequested()) {
            round_inflight_ = false;
            return;
        }
    }

    try {
        // 回收异常不影响在线决策；本轮必须释放 inflight，使后续 tick 能继续尝试。
        feature_store_->reclaimColdStates(nowMs());
    } catch (...) {
    }
    finishRound();
}

void FeatureStateMaintenance::finishRound() noexcept {
    std::lock_guard lock(mutex_);
    round_inflight_ = false;
    // 与黑名单维护一样，tick 重排遇到 Timer 瞬时拥塞时
    // 不得永久停止。任务结束后由 maintenance worker 补试。
    if (running_ && !active_timer_.valid()) {
        static_cast<void>(scheduleNextLocked());
    }
}

}  // 命名空间 aegisflow::app
