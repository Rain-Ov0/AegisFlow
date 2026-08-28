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

void cancellationDoesNotDependOnCommandCapacity() {
    using aegisflow::timer::SteadyClock;
    using aegisflow::timer::TimerCore;
    using aegisflow::timer::TimerCoreConfig;
    using aegisflow::timer::TimerEvent;
    using aegisflow::timer::TimerEventKind;
    using aegisflow::timer::TimerStatus;

    TimerCore core;
    TimerCoreConfig config;
    config.command_capacity = 1;
    config.timer_capacity = 1;
    require(core.init(config) == TimerStatus::Ok, "TimerCore 必须初始化");

    const auto sink = std::make_shared<RecordingTimerSink>();
    TimerEvent event;
    event.kind = TimerEventKind::CleanupTick;
    const auto first = core.scheduleAt(
        SteadyClock::now() + 1h,
        sink,
        event
    );
    require(first.status == TimerStatus::Ok, "首个 Timer 必须占满命令队列");
    require(
        core.cancel(first.id) == TimerStatus::Ok,
        "取消不得因 schedule command 已占满队列而失败"
    );
    require(
        core.processCommands().status == TimerStatus::Ok,
        "已取消且未入堆的 schedule command 应可安全丢弃"
    );

    const auto second = core.scheduleAt(
        SteadyClock::now() - 1ms,
        sink,
        event
    );
    require(
        second.status == TimerStatus::Ok,
        "取消后必须立即释放 timer capacity"
    );
    const auto ready = core.runReady();
    require(
        ready.status == TimerStatus::Ok && ready.timers_dispatched == 1 &&
            sink->deliveries.load(std::memory_order_relaxed) == 1,
        "新 Timer 应正常投递且旧 Timer 不得投递"
    );
}

}  // namespace

int main() {
    return aegisflow::test::runModule(
        "timer",
        {
            {"取消与重复停止", cancelledTimerIsNotDelivered},
            {"取消不依赖命令队列容量",
             cancellationDoesNotDependOnCommandCapacity},
        }
    );
}
