#include "aegisflow/timer/timer.hpp"

#include "tests/support/test_harness.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace {

using namespace std::chrono_literals;
using aegisflow::test::require;

class RecordingTimerSink final : public aegisflow::timer::ITimerSink {
public:
    bool tryPost(aegisflow::timer::TimerEvent) noexcept override {
        deliveries.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    std::atomic<int> deliveries{0};
};

void cancelledTimerIsNotDelivered() {
    using aegisflow::timer::Timer;
    using aegisflow::timer::TimerStatus;

    auto& timer = Timer::instance();
    aegisflow::timer::TimerConfig config;
    config.command_capacity = 16;
    config.timer_capacity = 16;
    require(
        timer.init(config) == TimerStatus::Ok,
        "Timer 必须完成初始化"
    );
    require(timer.start() == TimerStatus::Ok, "Timer 必须启动");

    const auto sink = std::make_shared<RecordingTimerSink>();
    aegisflow::timer::TimerEvent event;
    event.kind = aegisflow::timer::TimerEventKind::CleanupTick;
    const auto scheduled = timer.scheduleAt(
        aegisflow::timer::SteadyClock::now() + 120ms,
        sink,
        event
    );
    require(scheduled.status == TimerStatus::Ok, "Timer 任务必须成功调度");
    require(timer.cancel(scheduled.id) == TimerStatus::Ok, "Timer 取消必须成功");
    std::this_thread::sleep_for(220ms);
    require(
        sink->deliveries.load(std::memory_order_relaxed) == 0,
        "已取消 Timer 不得投递"
    );

    timer.stop();
    require(timer.join() == TimerStatus::Ok, "Timer 必须正常停止并 join");
    timer.stop();
    require(timer.join() == TimerStatus::Ok, "Timer 重复停止与 join 不得死锁");
}

}  // namespace

int main() {
    return aegisflow::test::runModule(
        "timer",
        {{"取消与重复停止", cancelledTimerIsNotDelivered}}
    );
}
