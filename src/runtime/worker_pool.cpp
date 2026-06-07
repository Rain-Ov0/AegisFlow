#include "aegisflow/runtime/worker_pool.hpp"

#include <algorithm>

namespace aegisflow::runtime {

WorkerPool::WorkerPool(size_t thread_num) {
    thread_num = std::max<size_t>(1, thread_num);

    threads_.reserve(thread_num);
    for (size_t i = 0; i < thread_num; ++ i ) {
        threads_.emplace_back([this](){
            workerLoop();
        });
    }
}

WorkerPool::~WorkerPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
    }

    cv_.notify_all();

    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void WorkerPool::workerLoop() {
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this](){
                return stopped_ || !tasks_.empty();
            }) ;

            if (stopped_ && tasks_.empty()) {
                return ;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        task();
    }
}

} // namespace aegisflow::runtime