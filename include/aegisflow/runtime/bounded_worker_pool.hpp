#pragma once

#include "aegisflow/runtime/cancellation.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace aegisflow::runtime {

enum class WorkerSubmitStatus : std::uint8_t {
    Accepted,
    Rejected,
    Closed,
};

enum class WorkerPoolStatus : std::uint8_t {
    Ok,
    InvalidState,
    DeadlineExceeded,
    SelfJoin,
};

struct BoundedWorkerPoolConfig {
    std::size_t thread_count = 4;
    std::size_t queue_capacity = 1024;

    [[nodiscard]] constexpr bool operator==(
        const BoundedWorkerPoolConfig& other
    ) const noexcept {
        return thread_count == other.thread_count &&
               queue_capacity == other.queue_capacity;
    }
};

class IWorkerTask {
public:
    virtual ~IWorkerTask() = default;

    // 任务对象以唯一所有权转移，执行期间不修改自身值负载。
    // 截止时间到达后只发出协作停止请求，任务必须自行观察令牌。
    virtual void run(CancellationToken stop_token) const = 0;
};

class BoundedWorkerPool final {
public:
    explicit BoundedWorkerPool(
        const BoundedWorkerPoolConfig& config
    );
    ~BoundedWorkerPool();

    BoundedWorkerPool(const BoundedWorkerPool&) = delete;
    BoundedWorkerPool& operator=(const BoundedWorkerPool&) = delete;
    BoundedWorkerPool(BoundedWorkerPool&&) = delete;
    BoundedWorkerPool& operator=(BoundedWorkerPool&&) = delete;

    [[nodiscard]] WorkerSubmitStatus trySubmit(
        std::unique_ptr<IWorkerTask> task
    );
    [[nodiscard]] std::uint64_t inflightCount() const noexcept;
    void close() noexcept;
    [[nodiscard]] WorkerPoolStatus drainUntil(
        std::chrono::steady_clock::time_point deadline
    ) noexcept;
    [[nodiscard]] WorkerPoolStatus join() noexcept;
private:
    enum class State : std::uint8_t {
        Running,
        Closing,
        Stopped,
    };

    [[nodiscard]] static bool validConfig(
        const BoundedWorkerPoolConfig& config
    ) noexcept;
    [[nodiscard]] bool isWorkerThreadLocked(
        std::thread::id thread_id
    ) const noexcept;
    void workerLoop(std::size_t worker_index) noexcept;

    BoundedWorkerPoolConfig config_;
    mutable std::mutex mutex_;
    std::condition_variable work_available_;
    std::condition_variable state_changed_;
    std::vector<std::unique_ptr<IWorkerTask>> queue_;
    std::vector<std::unique_ptr<IWorkerTask>> cancel_buffer_;
    std::vector<std::thread> threads_;
    std::vector<std::thread::id> worker_ids_;
    CancellationSource stop_source_;
    std::size_t queue_head_ = 0;
    std::size_t queue_tail_ = 0;
    std::size_t queue_size_ = 0;
    std::uint64_t inflight_ = 0;
    bool accepting_ = true;
    bool join_in_progress_ = false;
    WorkerPoolStatus join_status_ = WorkerPoolStatus::Ok;
    State state_ = State::Running;
};

}  // 命名空间 aegisflow::runtime
