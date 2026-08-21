#pragma once

#include "aegisflow/feature/feature_store.hpp"
#include "aegisflow/runtime/bounded_worker_pool.hpp"
#include "aegisflow/runtime/cancellation.hpp"
#include "aegisflow/timer/timer_core.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>

namespace aegisflow::app {

struct FeatureStateMaintenanceConfig {
    std::chrono::milliseconds cleanup_interval{
        std::chrono::minutes(1)
    };

    bool operator==(const FeatureStateMaintenanceConfig& other) const {
        return cleanup_interval == other.cleanup_interval;
    }
};

class FeatureStateMaintenance final
    : public timer::ITimerSink,
      public std::enable_shared_from_this<FeatureStateMaintenance> {
public:
    [[nodiscard]] static std::shared_ptr<FeatureStateMaintenance> create(
        FeatureStateMaintenanceConfig config,
        runtime::BoundedWorkerPool& maintenance_pool,
        timer::ITimerScheduler& timer_scheduler,
        feature::LoginFeatureStore& feature_store
    );

    ~FeatureStateMaintenance() override;

    FeatureStateMaintenance(const FeatureStateMaintenance&) = delete;
    FeatureStateMaintenance& operator=(const FeatureStateMaintenance&) =
        delete;
    FeatureStateMaintenance(FeatureStateMaintenance&&) = delete;
    FeatureStateMaintenance& operator=(FeatureStateMaintenance&&) = delete;

    [[nodiscard]] bool start() noexcept;
    void stop() noexcept;
    [[nodiscard]] bool tryPost(timer::TimerEvent event) noexcept override;

private:
    class CleanupTask;

    FeatureStateMaintenance(
        FeatureStateMaintenanceConfig config,
        runtime::BoundedWorkerPool& maintenance_pool,
        timer::ITimerScheduler& timer_scheduler,
        feature::LoginFeatureStore& feature_store
    );

    [[nodiscard]] bool scheduleNextLocked() noexcept;
    [[nodiscard]] bool submitRoundLocked() noexcept;
    void runRound(runtime::CancellationToken stop_token) noexcept;
    void finishRound() noexcept;

    FeatureStateMaintenanceConfig config_;
    runtime::BoundedWorkerPool* maintenance_pool_;
    timer::ITimerScheduler* timer_scheduler_;
    feature::LoginFeatureStore* feature_store_;
    mutable std::mutex mutex_;
    timer::TimerId active_timer_;
    std::uint64_t active_sequence_ = 0;
    std::uint64_t next_sequence_ = 1;
    bool running_ = false;
    bool stopped_ = false;
    bool round_inflight_ = false;
};

}  // 命名空间 aegisflow::app
