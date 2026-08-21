#include "aegisflow/net/event_loop_group.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace aegisflow::net {

namespace {

enum class EventLoopGroupState {
    Constructed,
    Starting,
    Running,
    Draining,
    Stopping,
    Stopped,
    Failed,
};

[[nodiscard]] EventLoopGroupConfig checkedGroupConfig(
    EventLoopGroupConfig config
) {
    const bool connection_topology_overflows =
        config.loop_count != 0 &&
        config.max_connections_per_loop >
            std::numeric_limits<std::size_t>::max() /
                config.loop_count;
    if (config.loop_count == 0 ||
        config.loop_count >
            static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max()
            ) ||
        config.max_connections == 0 ||
        config.max_connections_per_loop == 0 ||
        config.connection_queue_capacity == 0 ||
        config.connection_queue_capacity >
            config.max_connections_per_loop ||
        (!connection_topology_overflows &&
         config.max_connections >
             config.loop_count * config.max_connections_per_loop)) {
        throw std::invalid_argument("EventLoopGroup 配置无效");
    }
    return config;
}

class ConnectionLoadCounter final {
public:
    explicit ConnectionLoadCounter(
        std::shared_ptr<ConnectionLoadCounter> parent = nullptr
    ) noexcept : parent_(std::move(parent)) {}

    void setZeroNotification(
        std::condition_variable* notification
    ) noexcept {
        zero_notification_ = notification;
    }

    void reserve() noexcept {
        load_.fetch_add(1, std::memory_order_acq_rel);
        if (parent_ != nullptr) {
            parent_->reserve();
        }
    }

    void releaseOne() noexcept {
        release(1);
    }

    void release(const std::size_t count) noexcept {
        auto current = load_.load(std::memory_order_acquire);
        while (true) {
            const auto next = count >= current ? 0 : current - count;
            const auto released = current - next;
            if (load_.compare_exchange_weak(
                    current,
                    next,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire
                )) {
                if (parent_ != nullptr && released != 0) {
                    parent_->release(released);
                }
                if (next == 0 && released != 0 &&
                    zero_notification_ != nullptr) {
                    zero_notification_->notify_all();
                }
                return;
            }
        }
    }

    [[nodiscard]] std::size_t load() const noexcept {
        return load_.load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<ConnectionLoadCounter> parent_;
    std::condition_variable* zero_notification_ = nullptr;
    std::atomic<std::size_t> load_{0};
};

}  // 命名空间

class EventLoopGroup::Impl final {
public:
    struct LoopSlot {
        explicit LoopSlot(
            const std::uint32_t id,
            const std::size_t queue_capacity,
            std::shared_ptr<ConnectionLoadCounter> global_connection_load
        ) : loop_id(id),
            registrations(
                std::make_shared<ConnectionMailbox>(queue_capacity)
            ),
            connection_load(std::make_shared<ConnectionLoadCounter>(
                std::move(global_connection_load)
            )) {}

        std::uint32_t loop_id = 0;
        std::shared_ptr<ConnectionMailbox> registrations;
        std::shared_ptr<ConnectionLoadCounter> connection_load;
        std::thread thread;
        std::thread::id thread_id;
        bool ready = false;
        bool startup_failed = false;
    };

    Impl(
        EventLoopGroupConfig group_config,
        runtime::BoundedWorkerPool& pool,
        std::shared_ptr<IFrameBusinessHandler> handler,
        timer::ITimerScheduler* scheduler
    ) : config(checkedGroupConfig(std::move(group_config))),
        worker_pool(&pool),
        business_handler(std::move(handler)),
        timer_scheduler(scheduler),
        completion_router(std::make_shared<CompletionRouter>(
            0,
            config.loop_count
        )),
        drain_control(std::make_shared<EventLoopDrainControl>()) {
        if (business_handler == nullptr) {
            throw std::invalid_argument("EventLoopGroup 缺少业务处理器");
        }
        connection_load->setZeroNotification(&state_changed);
        loops.reserve(config.loop_count);
        for (std::size_t index = 0; index < config.loop_count; ++index) {
            loops.push_back(std::make_unique<LoopSlot>(
                static_cast<std::uint32_t>(index),
                config.connection_queue_capacity,
                connection_load
            ));
        }
    }

    ~Impl() {
        stop();
        (void)join();
    }

    void closeRegistrationMailboxes() noexcept {
        for (const auto& loop : loops) {
            const auto discarded = loop->registrations->close();
            if (discarded != 0) {
                loop->connection_load->release(discarded);
            }
        }
    }

    void loopMain(const std::size_t index) noexcept {
        auto& slot = *loops[index];
        std::unique_ptr<EventLoop> event_loop;
        try {
            auto loop_config = config.event_loop;
            loop_config.loop_id = slot.loop_id;
            loop_config.max_connections =
                config.max_connections_per_loop;
            event_loop = std::make_unique<EventLoop>(
                std::move(loop_config),
                *worker_pool,
                business_handler,
                *timer_scheduler,
                slot.registrations,
                [counter = slot.connection_load] {
                    counter->releaseOne();
                },
                completion_router,
                drain_control
            );
        } catch (...) {
            std::lock_guard lock(mutex);
            slot.thread_id = std::this_thread::get_id();
            slot.ready = true;
            slot.startup_failed = true;
            startup_failed = true;
            ++ready_count;
            state_changed.notify_all();
            return;
        }

        {
            std::unique_lock lock(mutex);
            slot.thread_id = std::this_thread::get_id();
            slot.ready = true;
            ++ready_count;
            state_changed.notify_all();
            state_changed.wait(lock, [this] {
                return state != EventLoopGroupState::Starting ||
                       stop_requested;
            });
        }

        bool failed = false;
        while (true) {
            {
                std::lock_guard lock(mutex);
                if (stop_requested) {
                    break;
                }
            }
            const auto status = event_loop->pollOnce(-1);
            if (status == EventLoopStatus::Ok) {
                continue;
            }
            if (status != EventLoopStatus::Stopped) {
                failed = true;
            }
            break;
        }

        (void)event_loop->shutdown();
        bool wake_other_loops = false;
        {
            std::lock_guard lock(mutex);
            if (failed) {
                loop_failed = true;
                stop_requested = true;
                state = EventLoopGroupState::Failed;
                wake_other_loops = true;
            }
            state_changed.notify_all();
        }
        if (wake_other_loops) {
            completion_router->close();
            closeRegistrationMailboxes();
        }
    }

    [[nodiscard]] EventLoopGroupStatus start() noexcept {
        std::unique_lock lock(mutex);
        if (state == EventLoopGroupState::Running) {
            return EventLoopGroupStatus::Ok;
        }
        if (state != EventLoopGroupState::Constructed) {
            return EventLoopGroupStatus::InvalidState;
        }

        state = EventLoopGroupState::Starting;
        ready_count = 0;
        startup_failed = false;
        stop_requested = false;
        std::size_t launched = 0;
        try {
            for (; launched < loops.size(); ++launched) {
                loops[launched]->thread = std::thread(
                    [this, launched] { loopMain(launched); }
                );
            }
        } catch (...) {
            startup_failed = true;
            stop_requested = true;
            state = EventLoopGroupState::Stopping;
        }

        if (!startup_failed) {
            state_changed.wait(lock, [this] {
                return ready_count == loops.size();
            });
        }

        if (!startup_failed && stop_requested) {
            return EventLoopGroupStatus::InvalidState;
        }

        if (startup_failed) {
            stop_requested = true;
            state = EventLoopGroupState::Stopping;
            lock.unlock();
            completion_router->close();
            closeRegistrationMailboxes();
            state_changed.notify_all();
            for (std::size_t index = 0; index < launched; ++index) {
                if (loops[index]->thread.joinable()) {
                    loops[index]->thread.join();
                }
            }
            lock.lock();
            state = EventLoopGroupState::Failed;
            join_status = EventLoopGroupStatus::StartFailed;
            return EventLoopGroupStatus::StartFailed;
        }

        state = EventLoopGroupState::Running;
        state_changed.notify_all();
        return EventLoopGroupStatus::Ok;
    }

    void stop() noexcept {
        bool close_mailboxes = false;
        {
            std::lock_guard lock(mutex);
            if (state == EventLoopGroupState::Constructed) {
                stop_requested = true;
                state = EventLoopGroupState::Stopped;
                close_mailboxes = true;
            } else if (state == EventLoopGroupState::Starting ||
                       state == EventLoopGroupState::Running ||
                       state == EventLoopGroupState::Draining) {
                stop_requested = true;
                state = EventLoopGroupState::Stopping;
                close_mailboxes = true;
            } else if (state == EventLoopGroupState::Failed) {
                stop_requested = true;
                close_mailboxes = true;
            }
        }
        if (close_mailboxes) {
            completion_router->close();
            closeRegistrationMailboxes();
            state_changed.notify_all();
        }
    }

    [[nodiscard]] EventLoopGroupStatus beginDrain() noexcept {
        bool close_registrations = false;
        {
            std::lock_guard lock(mutex);
            if (state == EventLoopGroupState::Draining) {
                return EventLoopGroupStatus::Ok;
            }
            if (state != EventLoopGroupState::Running) {
                return EventLoopGroupStatus::InvalidState;
            }
            state = EventLoopGroupState::Draining;
            drain_control->requestDrain();
            close_registrations = true;
        }
        if (close_registrations) {
            closeRegistrationMailboxes();
            state_changed.notify_all();
        }
        return EventLoopGroupStatus::Ok;
    }

    [[nodiscard]] EventLoopGroupStatus drainUntil(
        const std::chrono::steady_clock::time_point deadline
    ) noexcept {
        std::unique_lock lock(mutex);
        if (state != EventLoopGroupState::Draining) {
            return EventLoopGroupStatus::InvalidState;
        }
        if (connection_load->load() == 0) {
            return EventLoopGroupStatus::Ok;
        }
        const bool drained = state_changed.wait_until(
            lock,
            deadline,
            [this] {
                return connection_load->load() == 0 ||
                       state != EventLoopGroupState::Draining;
            }
        );
        if (drained && connection_load->load() == 0) {
            return EventLoopGroupStatus::Ok;
        }
        if (state != EventLoopGroupState::Draining) {
            return EventLoopGroupStatus::InvalidState;
        }
        return EventLoopGroupStatus::DrainDeadlineExceeded;
    }

    [[nodiscard]] EventLoopGroupStatus join() noexcept {
        std::vector<std::thread> joining_threads;
        {
            std::unique_lock lock(mutex);
            const auto caller = std::this_thread::get_id();
            for (const auto& loop : loops) {
                if (loop->thread_id == caller) {
                    return EventLoopGroupStatus::SelfJoin;
                }
            }
            if (state == EventLoopGroupState::Constructed ||
                state == EventLoopGroupState::Starting ||
                state == EventLoopGroupState::Running ||
                state == EventLoopGroupState::Draining) {
                return EventLoopGroupStatus::InvalidState;
            }
            if (join_in_progress) {
                state_changed.wait(lock, [this] {
                    return !join_in_progress;
                });
                return join_status;
            }

            for (auto& loop : loops) {
                if (loop->thread.joinable()) {
                    joining_threads.push_back(std::move(loop->thread));
                }
            }
            if (joining_threads.empty()) {
                if (state == EventLoopGroupState::Stopping) {
                    state = EventLoopGroupState::Stopped;
                }
                return join_status;
            }
            join_in_progress = true;
        }

        for (auto& thread : joining_threads) {
            thread.join();
        }

        {
            std::lock_guard lock(mutex);
            join_status = loop_failed
                              ? EventLoopGroupStatus::StartFailed
                              : EventLoopGroupStatus::Ok;
            if (!loop_failed && state != EventLoopGroupState::Failed) {
                state = EventLoopGroupState::Stopped;
            }
            join_in_progress = false;
            state_changed.notify_all();
            return join_status;
        }
    }

    void tryDispatch(OwnedSocket socket) noexcept {
        std::lock_guard lock(mutex);
        if (!socket.valid()) {
            return;
        }
        if (state != EventLoopGroupState::Running) {
            return;
        }
        if (connection_load->load() >= config.max_connections) {
            return;
        }

        const auto target_index = next_loop_index;
        next_loop_index = (next_loop_index + 1) % loops.size();
        auto& target = *loops[target_index];
        if (target.connection_load->load() >=
            config.max_connections_per_loop) {
            return;
        }
        target.connection_load->reserve();
        const bool posted = target.registrations->tryPost(
            std::move(socket)
        );
        if (!posted) {
            target.connection_load->releaseOne();
        }
    }

    EventLoopGroupConfig config;
    runtime::BoundedWorkerPool* worker_pool = nullptr;
    std::shared_ptr<IFrameBusinessHandler> business_handler;
    timer::ITimerScheduler* timer_scheduler = nullptr;
    std::shared_ptr<CompletionRouter> completion_router;
    std::shared_ptr<EventLoopDrainControl> drain_control;
    std::shared_ptr<ConnectionLoadCounter> connection_load =
        std::make_shared<ConnectionLoadCounter>();
    mutable std::mutex mutex;
    std::condition_variable state_changed;
    std::vector<std::unique_ptr<LoopSlot>> loops;
    EventLoopGroupState state = EventLoopGroupState::Constructed;
    EventLoopGroupStatus join_status = EventLoopGroupStatus::Ok;
    std::size_t ready_count = 0;
    std::size_t next_loop_index = 0;
    bool stop_requested = false;
    bool startup_failed = false;
    bool loop_failed = false;
    bool join_in_progress = false;
};

EventLoopGroup::EventLoopGroup(
    EventLoopGroupConfig config,
    runtime::BoundedWorkerPool& worker_pool,
    std::shared_ptr<IFrameBusinessHandler> business_handler,
    timer::ITimerScheduler& timer_scheduler
) : impl_(std::make_unique<Impl>(
        std::move(config),
        worker_pool,
        std::move(business_handler),
        &timer_scheduler
    )) {}

EventLoopGroup::~EventLoopGroup() = default;

EventLoopGroupStatus EventLoopGroup::start() noexcept {
    return impl_->start();
}

EventLoopGroupStatus EventLoopGroup::beginDrain() noexcept {
    return impl_->beginDrain();
}

EventLoopGroupStatus EventLoopGroup::drainUntil(
    const std::chrono::steady_clock::time_point deadline
) noexcept {
    return impl_->drainUntil(deadline);
}

void EventLoopGroup::stop() noexcept {
    impl_->stop();
}

EventLoopGroupStatus EventLoopGroup::join() noexcept {
    return impl_->join();
}

void EventLoopGroup::tryDispatch(OwnedSocket socket) noexcept {
    impl_->tryDispatch(std::move(socket));
}

}  // 命名空间 aegisflow::net
