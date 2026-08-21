#include "aegisflow/runtime/bounded_worker_pool.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>

namespace aegisflow::runtime {

BoundedWorkerPool::BoundedWorkerPool(
    const BoundedWorkerPoolConfig& config
) : config_(config) {
    if (!validConfig(config)) {
        throw std::invalid_argument("有界 WorkerPool 配置无效");
    }

    queue_.resize(config.queue_capacity);
    cancel_buffer_.resize(config.queue_capacity);
    threads_.reserve(config.thread_count);
    worker_ids_.resize(config.thread_count);
    try {
        for (std::size_t index = 0; index < config.thread_count; ++index) {
            threads_.emplace_back([this, index] { workerLoop(index); });
        }
    } catch (...) {
        close();
        (void)join();
        throw;
    }
}

BoundedWorkerPool::~BoundedWorkerPool() {
    close();
    if (join() != WorkerPoolStatus::Ok) {
        std::terminate();
    }
}

bool BoundedWorkerPool::validConfig(
    const BoundedWorkerPoolConfig& config
) noexcept {
    return config.thread_count > 0 && config.queue_capacity > 0;
}

WorkerSubmitStatus BoundedWorkerPool::trySubmit(
    std::unique_ptr<IWorkerTask> task
) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_) {
            return WorkerSubmitStatus::Closed;
        }
        if (task == nullptr) {
            return WorkerSubmitStatus::Rejected;
        }
        if (queue_size_ >= config_.queue_capacity) {
            return WorkerSubmitStatus::Rejected;
        }

        queue_[queue_tail_] = std::move(task);
        queue_tail_ = (queue_tail_ + 1) % config_.queue_capacity;
        ++queue_size_;
        ++inflight_;
    }
    work_available_.notify_one();
    return WorkerSubmitStatus::Accepted;
}

std::uint64_t BoundedWorkerPool::inflightCount() const noexcept {
    // inflight_ 从成功入队开始计数，直到执行完成或排队取消才归还；
    // reset barrier 因此不会漏掉尚未被 worker 取走的业务任务。
    std::lock_guard<std::mutex> lock(mutex_);
    return inflight_;
}

void BoundedWorkerPool::close() noexcept {
    // close 先停止接收新任务，已排队与在途任务仍由 drainUntil 按截止时间处理。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_) {
            return;
        }
        accepting_ = false;
        state_ = State::Closing;
        state_changed_.notify_all();
    }
    work_available_.notify_all();
}

WorkerPoolStatus BoundedWorkerPool::drainUntil(
    const std::chrono::steady_clock::time_point deadline
) noexcept {
    close();

    std::size_t cancelled_now = 0;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (state_ == State::Stopped || inflight_ == 0) {
            return WorkerPoolStatus::Ok;
        }
        if (state_changed_.wait_until(lock, deadline, [this]() {
                return inflight_ == 0;
            })) {
            return WorkerPoolStatus::Ok;
        }

        cancelled_now = queue_size_;
        for (std::size_t index = 0; index < cancelled_now; ++index) {
            cancel_buffer_[index] = std::move(queue_[queue_head_]);
            queue_head_ = (queue_head_ + 1) % config_.queue_capacity;
        }
        queue_size_ = 0;
        queue_tail_ = queue_head_;
        inflight_ -= cancelled_now;
        state_changed_.notify_all();
    }

    work_available_.notify_all();
    static_cast<void>(stop_source_.requestStop());
    for (std::size_t index = 0; index < cancelled_now; ++index) {
        cancel_buffer_[index].reset();
    }
    return WorkerPoolStatus::DeadlineExceeded;
}

WorkerPoolStatus BoundedWorkerPool::join() noexcept {
    std::vector<std::thread> threads;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (isWorkerThreadLocked(std::this_thread::get_id())) {
            return WorkerPoolStatus::SelfJoin;
        }
        if (state_ == State::Running) {
            return WorkerPoolStatus::InvalidState;
        }
        if (join_in_progress_) {
            state_changed_.wait(lock, [this]() {
                return !join_in_progress_;
            });
            return join_status_;
        }
        if (state_ == State::Stopped) {
            return join_status_;
        }

        join_in_progress_ = true;
        threads = std::move(threads_);
    }

    for (auto& thread : threads) {
        if (!thread.joinable()) {
            continue;
        }
        try {
            thread.join();
        } catch (...) {
            std::terminate();
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = State::Stopped;
        join_status_ = WorkerPoolStatus::Ok;
        join_in_progress_ = false;
        state_changed_.notify_all();
    }
    return WorkerPoolStatus::Ok;
}

bool BoundedWorkerPool::isWorkerThreadLocked(
    const std::thread::id thread_id
) const noexcept {
    return std::find(
        worker_ids_.begin(),
        worker_ids_.end(),
        thread_id
    ) != worker_ids_.end();
}

void BoundedWorkerPool::workerLoop(
    const std::size_t worker_index
) noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_ids_[worker_index] = std::this_thread::get_id();
        state_changed_.notify_all();
    }

    while (true) {
        std::unique_ptr<IWorkerTask> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            work_available_.wait(lock, [this]() {
                return !accepting_ || queue_size_ > 0;
            });
            if (queue_size_ == 0) {
                if (!accepting_) {
                    worker_ids_[worker_index] = std::thread::id{};
                    state_changed_.notify_all();
                    return;
                }
                continue;
            }

            task = std::move(queue_[queue_head_]);
            queue_head_ = (queue_head_ + 1) % config_.queue_capacity;
            --queue_size_;
        }

        try {
            task->run(stop_source_.token());
        } catch (...) {
            // 单个任务失败不得终止 Worker 线程，inflight 仍在下方成对归还。
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            --inflight_;
            state_changed_.notify_all();
        }
    }
}

}  // 命名空间 aegisflow::runtime
