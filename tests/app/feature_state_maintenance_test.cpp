#include "aegisflow/app/feature_state_maintenance.hpp"
#include "aegisflow/feature/feature_store.hpp"
#include "aegisflow/runtime/bounded_worker_pool.hpp"
#include "aegisflow/timer/timer_core.hpp"

#include "tests/support/test_harness.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

using aegisflow::test::require;

class FakeTimerScheduler final : public aegisflow::timer::ITimerScheduler {
public:
    explicit FakeTimerScheduler(
        std::deque<aegisflow::timer::TimerStatus> statuses
    ) : statuses_(std::move(statuses)) {}

    aegisflow::timer::TimerScheduleResult scheduleAt(
        aegisflow::timer::SteadyTime,
        std::weak_ptr<aegisflow::timer::ITimerSink>,
        const aegisflow::timer::TimerEvent event
    ) noexcept override {
        std::lock_guard lock(mutex_);
        auto status = aegisflow::timer::TimerStatus::Ok;
        if (!statuses_.empty()) {
            status = statuses_.front();
            statuses_.pop_front();
        }
        if (status != aegisflow::timer::TimerStatus::Ok) {
            return {status, {}};
        }
        events_.push_back(event);
        aegisflow::timer::TimerId id;
        id.value = next_id_++;
        id.generation = id.value;
        return {aegisflow::timer::TimerStatus::Ok, id};
    }

    aegisflow::timer::TimerStatus cancel(
        aegisflow::timer::TimerId
    ) noexcept override {
        return aegisflow::timer::TimerStatus::Ok;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return events_.size();
    }

    [[nodiscard]] aegisflow::timer::TimerEvent event(
        const std::size_t index
    ) const {
        std::lock_guard lock(mutex_);
        return events_.at(index);
    }

private:
    mutable std::mutex mutex_;
    std::deque<aegisflow::timer::TimerStatus> statuses_;
    std::vector<aegisflow::timer::TimerEvent> events_;
    std::uint64_t next_id_ = 1;
};

void transientTimerQueueFullIsRetriedAfterRound() {
    using namespace std::chrono_literals;

    aegisflow::runtime::BoundedWorkerPool maintenance_pool({1, 2});
    aegisflow::feature::LoginFeatureStore feature_store;
    FakeTimerScheduler scheduler({
        aegisflow::timer::TimerStatus::Ok,
        aegisflow::timer::TimerStatus::QueueFull,
        aegisflow::timer::TimerStatus::Ok,
    });
    auto maintenance = aegisflow::app::FeatureStateMaintenance::create(
        {1ms}, maintenance_pool, scheduler, feature_store);

    require(
        maintenance->start() && scheduler.size() == 1,
        "首个 FeatureStateMaintenance tick 必须排程成功"
    );
    require(
        maintenance->tryPost(scheduler.event(0)),
        "tick 重排遇到 QueueFull 时仍应提交当前回收轮"
    );

    const auto limit = std::chrono::steady_clock::now() + 1s;
    while (scheduler.size() < 2 &&
           std::chrono::steady_clock::now() < limit) {
        std::this_thread::yield();
    }
    require(
        scheduler.size() == 2,
        "回收轮完成后必须重试瞬时 QueueFull 的 tick"
    );

    maintenance->stop();
    maintenance_pool.close();
    require(
        maintenance_pool.drainUntil(
            std::chrono::steady_clock::now() + 1s
        ) == aegisflow::runtime::WorkerPoolStatus::Ok,
        "FeatureStateMaintenance 测试任务必须排空"
    );
    require(
        maintenance_pool.join() ==
            aegisflow::runtime::WorkerPoolStatus::Ok,
        "FeatureStateMaintenance 测试 worker 必须退出"
    );
}

}  // namespace

int main() {
    return aegisflow::test::runModule(
        "feature_state_maintenance",
        {{"transient timer queue full is retried",
          transientTimerQueueFullIsRetriedAfterRound}}
    );
}
