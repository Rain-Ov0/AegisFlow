#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace aegisflow::runtime {

class WorkerPool {
public:
    explicit WorkerPool(size_t thread_num);
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    template <typename Func>
    auto submit(Func&& func)
        -> std::future<std::invoke_result_t<std::decay_t<Func>&>> {
        using Task = std::decay_t<Func>;
        using Ret = std::invoke_result_t<Task&>;

        auto task = std::make_shared<std::packaged_task<Ret()>>(
            std::forward<Func>(func)
        );

        auto future = task->get_future();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_) {
                throw std::runtime_error("submit task to stopped WorkerPool");
            }

            tasks_.emplace([task](){
                (*task)();
            });
        }

        cv_.notify_one();
        return future;
        }

private:
        void workerLoop();
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> threads_;
    bool stopped_ = false;
};

} // namespace aegisflow::runtime
