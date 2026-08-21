#include "aegisflow/runtime/bounded_worker_pool.hpp"

#include "tests/support/test_harness.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;
using aegisflow::test::require;

[[nodiscard]] aegisflow::runtime::BoundedWorkerPoolConfig poolConfig(
    const std::size_t queue_capacity
) {
    aegisflow::runtime::BoundedWorkerPoolConfig config;
    config.thread_count = 1;
    config.queue_capacity = queue_capacity;
    return config;
}

struct BlockingGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool running = false;
    bool released = false;
};

struct CancellationGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool running = false;
    bool observed = false;
};

class BlockingTask final : public aegisflow::runtime::IWorkerTask {
public:
    explicit BlockingTask(std::shared_ptr<BlockingGate> gate)
        : gate_(std::move(gate)) {}

    void run(aegisflow::runtime::CancellationToken) const override {
        std::unique_lock<std::mutex> lock(gate_->mutex);
        gate_->running = true;
        gate_->changed.notify_all();
        gate_->changed.wait(lock, [this] { return gate_->released; });
    }

private:
    std::shared_ptr<BlockingGate> gate_;
};

class CountingTask final : public aegisflow::runtime::IWorkerTask {
public:
    explicit CountingTask(std::atomic<int>& count) : count_(count) {}

    void run(aegisflow::runtime::CancellationToken) const override {
        count_.fetch_add(1, std::memory_order_relaxed);
    }

private:
    std::atomic<int>& count_;
};

class ThrowingTask final : public aegisflow::runtime::IWorkerTask {
public:
    void run(aegisflow::runtime::CancellationToken) const override {
        throw std::runtime_error("expected worker task failure");
    }
};

class CancellationAwareTask final : public aegisflow::runtime::IWorkerTask {
public:
    explicit CancellationAwareTask(std::shared_ptr<CancellationGate> gate)
        : gate_(std::move(gate)) {}

    void run(
        const aegisflow::runtime::CancellationToken stop_token
    ) const override {
        {
            std::lock_guard<std::mutex> lock(gate_->mutex);
            gate_->running = true;
        }
        gate_->changed.notify_all();

        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!stop_token.stopRequested() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        {
            std::lock_guard<std::mutex> lock(gate_->mutex);
            gate_->observed = stop_token.stopRequested();
        }
        gate_->changed.notify_all();
    }

private:
    std::shared_ptr<CancellationGate> gate_;
};

class StopRecordingTask final : public aegisflow::runtime::IWorkerTask {
public:
    StopRecordingTask(std::atomic<bool>& ran, std::atomic<bool>& saw_stop)
        : ran_(ran), saw_stop_(saw_stop) {}

    void run(
        const aegisflow::runtime::CancellationToken stop_token
    ) const override {
        saw_stop_.store(stop_token.stopRequested(), std::memory_order_relaxed);
        ran_.store(true, std::memory_order_release);
    }

private:
    std::atomic<bool>& ran_;
    std::atomic<bool>& saw_stop_;
};

void boundedQueueRejectsOverloadAndDrainsAcceptedWork() {
    using aegisflow::runtime::BoundedWorkerPool;
    using aegisflow::runtime::WorkerPoolStatus;
    using aegisflow::runtime::WorkerSubmitStatus;

    BoundedWorkerPool pool(poolConfig(1));
    const auto gate = std::make_shared<BlockingGate>();
    require(
        pool.trySubmit(std::make_unique<BlockingTask>(gate)) ==
            WorkerSubmitStatus::Accepted,
        "第一个 Worker 任务必须被接受"
    );
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        require(
            gate->changed.wait_for(lock, 2s, [&gate] { return gate->running; }),
            "Worker 未在期限内开始任务"
        );
    }

    std::atomic<int> completed{0};
    require(
        pool.trySubmit(std::make_unique<CountingTask>(completed)) ==
            WorkerSubmitStatus::Accepted,
        "队列空位必须接受任务"
    );
    require(
        pool.trySubmit(std::make_unique<CountingTask>(completed)) ==
            WorkerSubmitStatus::Rejected,
        "队列满时必须拒绝过载任务"
    );

    pool.close();
    require(
        pool.trySubmit(std::make_unique<CountingTask>(completed)) ==
            WorkerSubmitStatus::Closed,
        "关闭后不得接受新任务"
    );
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->released = true;
    }
    gate->changed.notify_all();
    require(
        pool.drainUntil(std::chrono::steady_clock::now() + 2s) ==
            WorkerPoolStatus::Ok,
        "关闭时已接受任务必须排空"
    );
    require(pool.join() == WorkerPoolStatus::Ok, "Worker 必须正常 join");
    require(completed.load(std::memory_order_relaxed) == 1, "排队任务必须执行一次");
}

void closeAndJoinAreIdempotent() {
    using aegisflow::runtime::BoundedWorkerPool;
    using aegisflow::runtime::WorkerPoolStatus;

    BoundedWorkerPool pool(poolConfig(1));
    pool.close();
    pool.close();
    require(pool.join() == WorkerPoolStatus::Ok, "首次 join 必须成功");
    require(pool.join() == WorkerPoolStatus::Ok, "重复 join 必须保持成功");
}

void inflightTracksEveryAcceptedTaskUntilItsTerminalState() {
    using aegisflow::runtime::BoundedWorkerPool;
    using aegisflow::runtime::WorkerPoolStatus;
    using aegisflow::runtime::WorkerSubmitStatus;

    BoundedWorkerPool pool(poolConfig(2));
    const auto gate = std::make_shared<BlockingGate>();
    require(
        pool.trySubmit(std::make_unique<BlockingTask>(gate)) ==
            WorkerSubmitStatus::Accepted,
        "运行中任务必须入队"
    );
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        require(
            gate->changed.wait_for(lock, 2s, [&gate] { return gate->running; }),
            "阻塞任务未开始运行"
        );
    }
    require(pool.inflightCount() == 1,
            "已取出并正在运行的任务必须计入 inflight");

    std::atomic<int> queued_runs{0};
    require(
        pool.trySubmit(std::make_unique<CountingTask>(queued_runs)) ==
            WorkerSubmitStatus::Accepted,
        "等待中任务必须入队"
    );
    require(pool.inflightCount() == 2 &&
                queued_runs.load(std::memory_order_relaxed) == 0,
            "尚未运行的排队任务必须计入 inflight");

    require(
        pool.drainUntil(std::chrono::steady_clock::now()) ==
            WorkerPoolStatus::DeadlineExceeded,
        "过期 deadline 必须触发排队取消"
    );
    require(pool.inflightCount() == 1 &&
                queued_runs.load(std::memory_order_relaxed) == 0,
            "deadline 取消只归还排队项，运行中任务仍在途");

    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->released = true;
    }
    gate->changed.notify_all();
    require(
        pool.drainUntil(std::chrono::steady_clock::now() + 2s) ==
            WorkerPoolStatus::Ok,
        "运行中任务释放后必须完成"
    );
    require(pool.inflightCount() == 0,
            "执行完成与排队取消后 inflight 必须归零");
    require(pool.join() == WorkerPoolStatus::Ok, "Worker 必须正常 join");

    BoundedWorkerPool throwing_pool(poolConfig(1));
    require(
        throwing_pool.trySubmit(std::make_unique<ThrowingTask>()) ==
            WorkerSubmitStatus::Accepted,
        "异常任务必须入队"
    );
    throwing_pool.close();
    require(
        throwing_pool.drainUntil(std::chrono::steady_clock::now() + 2s) ==
            WorkerPoolStatus::Ok,
        "任务异常不得阻止排空"
    );
    require(throwing_pool.inflightCount() == 0,
            "任务异常终态必须归还 inflight");
    require(throwing_pool.join() == WorkerPoolStatus::Ok,
            "异常任务后 Worker 必须正常 join");
}

void deadlineCancellationReachesRunningTask() {
    using aegisflow::runtime::BoundedWorkerPool;
    using aegisflow::runtime::BoundedWorkerPoolConfig;
    using aegisflow::runtime::WorkerPoolStatus;
    using aegisflow::runtime::WorkerSubmitStatus;

    BoundedWorkerPoolConfig config;
    config.thread_count = 1;
    config.queue_capacity = 1;
    BoundedWorkerPool pool(config);
    const auto gate = std::make_shared<CancellationGate>();
    require(
        pool.trySubmit(std::make_unique<CancellationAwareTask>(gate)) ==
            WorkerSubmitStatus::Accepted,
        "取消观察任务必须被接受"
    );
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        require(
            gate->changed.wait_for(lock, 2s, [&gate] {
                return gate->running;
            }),
            "取消观察任务未在期限内开始"
        );
    }

    require(
        pool.drainUntil(std::chrono::steady_clock::now()) ==
            WorkerPoolStatus::DeadlineExceeded,
        "已过期 drain 必须请求运行中任务停止"
    );
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        require(
            gate->changed.wait_for(lock, 2s, [&gate] {
                return gate->observed;
            }),
            "运行中任务未观察到协作停止"
        );
    }
    require(
        pool.drainUntil(std::chrono::steady_clock::now() + 2s) ==
            WorkerPoolStatus::Ok,
        "观察停止后 WorkerPool 必须排空"
    );
    require(pool.join() == WorkerPoolStatus::Ok,
            "取消后 WorkerPool 必须正常 join");
}

void normalDrainDoesNotRequestCancellation() {
    using aegisflow::runtime::BoundedWorkerPool;
    using aegisflow::runtime::BoundedWorkerPoolConfig;
    using aegisflow::runtime::WorkerPoolStatus;
    using aegisflow::runtime::WorkerSubmitStatus;

    BoundedWorkerPoolConfig config;
    config.thread_count = 1;
    config.queue_capacity = 1;
    BoundedWorkerPool pool(config);
    std::atomic<bool> ran{false};
    std::atomic<bool> saw_stop{false};
    require(
        pool.trySubmit(std::make_unique<StopRecordingTask>(ran, saw_stop)) ==
            WorkerSubmitStatus::Accepted,
        "正常 drain 任务必须被接受"
    );
    pool.close();
    require(
        pool.drainUntil(std::chrono::steady_clock::now() + 2s) ==
            WorkerPoolStatus::Ok,
        "正常 drain 必须排空任务"
    );
    require(pool.join() == WorkerPoolStatus::Ok,
            "正常 drain 后 WorkerPool 必须 join");
    require(ran.load(std::memory_order_acquire), "正常 drain 必须执行任务");
    require(!saw_stop.load(std::memory_order_relaxed),
            "正常 drain 不得误发停止请求");
}

}  // namespace

int main() {
    return aegisflow::test::runModule(
        "bounded_worker_pool",
        {
            {"过载与排空", boundedQueueRejectsOverloadAndDrainsAcceptedWork},
            {"重复关闭", closeAndJoinAreIdempotent},
            {"inflight 完整计数",
             inflightTracksEveryAcceptedTaskUntilItsTerminalState},
            {"deadline 协作取消", deadlineCancellationReachesRunningTask},
            {"正常 drain 不取消", normalDrainDoesNotRequestCancellation},
        }
    );
}
