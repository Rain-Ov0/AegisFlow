#include "aegisflow/net/event_loop.hpp"

#include "aegisflow/net/protocol_contract.hpp"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <atomic>
#include <limits>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace aegisflow::net {
namespace {

class LinuxSessionConnection final : public ISessionConnection {
public:
    explicit LinuxSessionConnection(const int fd) noexcept
        : fd_(fd) {}

    ~LinuxSessionConnection() override {
        close();
    }

    [[nodiscard]] int fd() const noexcept override {
        return fd_;
    }

    void close() noexcept override {
        if (fd_ < 0) {
            return;
        }
        const int closing_fd = fd_;
        fd_ = -1;
        (void)::close(closing_fd);
    }

private:
    int fd_ = -1;
};

// SessionPool 只负责同一 EventLoop 内的构造和销毁，不参与调度。
class SessionPool final {
public:
    struct Deleter {
        std::pmr::memory_resource* resource =
            std::pmr::get_default_resource();

        void operator()(Session* session) const noexcept {
            if (session == nullptr) {
                return;
            }
            session->~Session();
            std::pmr::polymorphic_allocator<Session>{resource}.deallocate(
                session, 1);
        }
    };

    using Pointer = std::unique_ptr<Session, Deleter>;

    explicit SessionPool(std::pmr::memory_resource* resource)
        : resource_(resource) {}

    [[nodiscard]] Pointer create(
        const ConnectionToken token,
        const SessionConfig config,
        std::unique_ptr<ISessionConnection> connection
    ) {
        std::pmr::polymorphic_allocator<Session> allocator{resource_};
        Session* session = allocator.allocate(1);
        try {
            ::new (static_cast<void*>(session)) Session(
                token,
                config,
                std::move(connection),
                resource_
            );
        } catch (...) {
            allocator.deallocate(session, 1);
            throw;
        }
        return Pointer(session, Deleter{resource_});
    }

private:
    std::pmr::memory_resource* resource_;
};

[[nodiscard]] EventLoopConfig checkedEventLoopConfig(
    EventLoopConfig config
) {
    if (config.max_events == 0 ||
        config.read_scratch_bytes == 0 ||
        config.max_connections == 0 ||
        config.completion_capacity == 0 ||
        config.completion_byte_capacity == 0 ||
        config.max_completion_bytes <= protocol::kFrameHeaderSize ||
        config.max_completion_bytes >
            config.session.max_output_buffer_bytes) {
        throw std::invalid_argument("EventLoop 配置无效");
    }
    return config;
}

[[nodiscard]] std::uint32_t epollEventsFor(
    const SessionInterest interest
) noexcept {
    std::uint32_t events = EPOLLET;
    if (hasInterest(interest, SessionInterest::Read)) {
        events |= EPOLLIN;
    }
    if (hasInterest(interest, SessionInterest::Write)) {
        events |= EPOLLOUT;
    }
    if (hasInterest(interest, SessionInterest::PeerReadClose)) {
        events |= EPOLLRDHUP;
    }
    return events;
}

[[nodiscard]] timer::TimerEvent sessionTimerEvent(
    const timer::TimerEventKind kind,
    const ConnectionToken token,
    const std::uint64_t sequence
) noexcept {
    timer::TimerEvent event;
    event.kind = kind;
    event.target_id = static_cast<std::uint64_t>(token.fd);
    event.target_loop_id = token.loop_id;
    event.target_fd = token.fd;
    event.target_generation = token.generation;
    event.target_sequence = sequence;
    return event;
}

}  // 命名空间

class EventLoop::Impl final {
public:
    struct SessionEntry {
        SessionPool::Pointer session;
        std::uint32_t registered_events = 0;
        std::uint64_t deadline_sequence = 0;
        timer::TimerEventKind deadline_kind =
            timer::TimerEventKind::IdleTimeout;
        timer::SteadyTime deadline_at{};
        bool deadline_active = false;
        bool business_expired = false;
    };

    Impl(
        EventLoopConfig loop_config,
        runtime::BoundedWorkerPool& pool,
        std::shared_ptr<IFrameBusinessHandler> handler,
        std::shared_ptr<ConnectionMailbox> registration_queue,
        std::function<void()> release_connection_callback,
        std::shared_ptr<CompletionRouter> router,
        std::shared_ptr<EventLoopDrainControl> shutdown_control,
        timer::ITimerScheduler* scheduler
    ) : config(checkedEventLoopConfig(std::move(loop_config))),
        worker_pool(&pool),
        business_handler(std::move(handler)),
        connection_mailbox(std::move(registration_queue)),
        release_connection(std::move(release_connection_callback)),
        completion_mailbox(
            std::make_shared<LoopMailbox>(
                config.completion_capacity,
                config.completion_byte_capacity,
                config.max_completion_bytes
            )
        ),
        timeout_mailbox(
            config.deadlines.enabled
                ? std::make_shared<TimeoutMailbox>(
                      config.max_connections
                  )
                : nullptr
        ),
        completion_router(std::move(router)),
        drain_control(std::move(shutdown_control)),
        timer_scheduler(scheduler),
        events(config.max_events),
        read_scratch(config.read_scratch_bytes),
        owner_thread(std::this_thread::get_id()) {
        if (business_handler == nullptr) {
            throw std::invalid_argument("EventLoop 缺少业务处理器");
        }
        if (config.deadlines.enabled && timer_scheduler == nullptr) {
            throw std::invalid_argument("EventLoop 启用 deadline 时缺少 Timer");
        }

        if (completion_router == nullptr) {
            throw std::invalid_argument("EventLoop 缺少 completion router");
        }
        if (!completion_router->bind(
                config.loop_id,
                completion_mailbox
            )) {
            throw std::invalid_argument("EventLoop completion router 绑定失败");
        }
        router_bound = true;

        epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd < 0) {
            unbindCompletionRouter();
            throw std::system_error(
                errno,
                std::generic_category(),
                "创建 EventLoop epoll 失败"
            );
        }

        epoll_event event{};
        event.events = EPOLLIN | EPOLLET;
        event.data.u64 = 0;
        if (::epoll_ctl(
                epoll_fd,
                EPOLL_CTL_ADD,
                completion_mailbox->wakeFd(),
                &event
            ) != 0) {
            const int saved_errno = errno;
            (void)::close(epoll_fd);
            epoll_fd = -1;
            unbindCompletionRouter();
            throw std::system_error(
                saved_errno,
                std::generic_category(),
                "注册 EventLoop mailbox 失败"
            );
        }

        if (connection_mailbox != nullptr &&
            ::epoll_ctl(
                epoll_fd,
                EPOLL_CTL_ADD,
                connection_mailbox->wakeFd(),
                &event
            ) != 0) {
            const int saved_errno = errno;
            (void)::close(epoll_fd);
            epoll_fd = -1;
            unbindCompletionRouter();
            throw std::system_error(
                saved_errno,
                std::generic_category(),
                "注册 EventLoop 新连接 mailbox 失败"
            );
        }

        if (timeout_mailbox != nullptr &&
            ::epoll_ctl(
                epoll_fd,
                EPOLL_CTL_ADD,
                timeout_mailbox->wakeFd(),
                &event
            ) != 0) {
            const int saved_errno = errno;
            (void)::close(epoll_fd);
            epoll_fd = -1;
            unbindCompletionRouter();
            throw std::system_error(
                saved_errno,
                std::generic_category(),
                "注册 EventLoop 超时 mailbox 失败"
            );
        }
    }

    ~Impl() {
        forceShutdown();
    }

    [[nodiscard]] bool onOwnerThread() const noexcept {
        return owner_thread == std::this_thread::get_id();
    }

    void unbindCompletionRouter() noexcept {
        if (!router_bound || completion_router == nullptr) {
            return;
        }
        completion_router->unbind(
            config.loop_id,
            completion_mailbox
        );
        router_bound = false;
    }

    [[nodiscard]] std::optional<std::uint64_t> allocateGeneration() noexcept {
        if (next_generation == 0) {
            return std::nullopt;
        }

        const auto generation = next_generation;
        if (next_generation == std::numeric_limits<std::uint64_t>::max()) {
            next_generation = 0;
        } else {
            ++next_generation;
        }
        return generation;
    }

    [[nodiscard]] static SessionTimerKind sessionTimerKind(
        const timer::TimerEventKind kind
    ) noexcept {
        switch (kind) {
            case timer::TimerEventKind::IdleTimeout:
                return SessionTimerKind::Idle;
            case timer::TimerEventKind::ReadTimeout:
                return SessionTimerKind::Read;
            case timer::TimerEventKind::WriteTimeout:
                return SessionTimerKind::Write;
            case timer::TimerEventKind::BusinessTimeout:
                return SessionTimerKind::Business;
            case timer::TimerEventKind::CleanupTick:
            case timer::TimerEventKind::BlacklistMaintenanceTick:
                return SessionTimerKind::Idle;
        }
        return SessionTimerKind::Idle;
    }

    [[nodiscard]] std::chrono::milliseconds timeoutFor(
        const timer::TimerEventKind kind
    ) const noexcept {
        switch (kind) {
            case timer::TimerEventKind::IdleTimeout:
                return config.deadlines.idle_timeout;
            case timer::TimerEventKind::ReadTimeout:
                return config.deadlines.read_timeout;
            case timer::TimerEventKind::WriteTimeout:
                return config.deadlines.write_timeout;
            case timer::TimerEventKind::BusinessTimeout:
                return config.deadlines.business_timeout;
            case timer::TimerEventKind::CleanupTick:
            case timer::TimerEventKind::BlacklistMaintenanceTick:
                return std::chrono::milliseconds(0);
        }
        return std::chrono::milliseconds(0);
    }

    void cancelTimerId(const timer::TimerId id) noexcept {
        if (!id.valid() || timer_scheduler == nullptr) {
            return;
        }
        (void)timer_scheduler->cancel(id);
    }

    void cancelDeadline(SessionEntry& entry) noexcept {
        if (!entry.deadline_active) {
            return;
        }
        const auto slot = sessionTimerKind(entry.deadline_kind);
        const auto id = entry.session->replaceTimer(slot, {});
        entry.deadline_active = false;
        cancelTimerId(id);
    }

    [[nodiscard]] bool armDeadline(
        SessionEntry& entry,
        const timer::TimerEventKind kind
    ) noexcept {
        if (!config.deadlines.enabled) {
            return true;
        }
        cancelDeadline(entry);
        if (entry.deadline_sequence ==
            std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        ++entry.deadline_sequence;
        const auto deadline = timer::SteadyClock::now() + timeoutFor(kind);
        const auto token = entry.session->token();
        const auto scheduled = timer_scheduler->scheduleAt(
            deadline,
            std::weak_ptr<timer::ITimerSink>(timeout_mailbox),
            sessionTimerEvent(kind, token, entry.deadline_sequence)
        );
        if (scheduled.status != timer::TimerStatus::Ok) {
            return false;
        }
        (void)entry.session->replaceTimer(
            sessionTimerKind(kind),
            scheduled.id
        );
        entry.deadline_kind = kind;
        entry.deadline_at = deadline;
        entry.deadline_active = true;
        return true;
    }

    [[nodiscard]] timer::TimerEventKind deadlineKindFor(
        const Session& session
    ) const noexcept {
        switch (session.state()) {
            case SessionState::Reading:
                return session.bufferedInputBytes() == 0
                           ? timer::TimerEventKind::IdleTimeout
                           : timer::TimerEventKind::ReadTimeout;
            case SessionState::Processing:
                return timer::TimerEventKind::BusinessTimeout;
            case SessionState::Writing:
                return timer::TimerEventKind::WriteTimeout;
            case SessionState::Closing:
            case SessionState::Closed:
                return timer::TimerEventKind::IdleTimeout;
        }
        return timer::TimerEventKind::IdleTimeout;
    }

    [[nodiscard]] bool syncDeadline(const int fd) noexcept {
        const auto it = sessions.find(fd);
        if (it == sessions.end() || !config.deadlines.enabled) {
            return it != sessions.end();
        }
        const auto state = it->second.session->state();
        if (state == SessionState::Closing || state == SessionState::Closed) {
            cancelDeadline(it->second);
            return true;
        }
        const auto desired = deadlineKindFor(*it->second.session);
        if (it->second.deadline_active &&
            it->second.deadline_kind == desired) {
            return true;
        }
        return armDeadline(it->second, desired);
    }

    [[nodiscard]] EventLoopStatus adopt(OwnedSocket socket) noexcept {
        if (!onOwnerThread()) {
            return EventLoopStatus::WrongThread;
        }
        if (stopped) {
            return EventLoopStatus::Stopped;
        }
        if (!socket.valid()) {
            return EventLoopStatus::InvalidArgument;
        }

        const int fd = socket.get();
        if (sessions.find(fd) != sessions.end()) {
            return EventLoopStatus::AlreadyRegistered;
        }
        if (sessions.size() >= config.max_connections) {
            return EventLoopStatus::ConnectionLimit;
        }

        const auto generation = allocateGeneration();
        if (!generation.has_value()) {
            return EventLoopStatus::GenerationExhausted;
        }
        ConnectionToken token;
        token.loop_id = config.loop_id;
        token.fd = fd;
        token.generation = *generation;

        bool inserted_session = false;
        bool inserted_event = false;
        try {
            auto connection = std::make_unique<LinuxSessionConnection>(fd);
            (void)socket.release();
            auto session = session_pool.create(
                token,
                config.session,
                std::move(connection)
            );
            const auto registered_events = epollEventsFor(session->interest());

            SessionEntry entry;
            entry.session = std::move(session);
            entry.registered_events = registered_events;
            auto [session_it, session_inserted] = sessions.emplace(
                fd,
                std::move(entry)
            );
            if (!session_inserted) {
                return EventLoopStatus::AlreadyRegistered;
            }
            inserted_session = true;

            auto [event_it, event_inserted] = event_to_fd.emplace(*generation, fd);
            (void)event_it;
            if (!event_inserted) {
                sessions.erase(session_it);
                inserted_session = false;
                return EventLoopStatus::SystemCallFailed;
            }
            inserted_event = true;

            epoll_event event{};
            event.events = registered_events;
            event.data.u64 = *generation;
            if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) != 0) {
                event_to_fd.erase(*generation);
                sessions.erase(fd);
                return EventLoopStatus::SystemCallFailed;
            }

            if (!syncDeadline(fd)) {
                closeSession(
                    fd,
                    SessionCloseReason::ResourceExhausted,
                    false
                );
                return EventLoopStatus::TimerScheduleFailed;
            }

            return EventLoopStatus::Ok;
        } catch (...) {
            if (inserted_event) {
                event_to_fd.erase(*generation);
            }
            if (inserted_session) {
                sessions.erase(fd);
            }
            return EventLoopStatus::SystemCallFailed;
        }
    }

    [[nodiscard]] bool modifyInterest(const int fd) noexcept {
        const auto it = sessions.find(fd);
        if (it == sessions.end()) {
            return false;
        }
        if (it->second.session->state() == SessionState::Closing ||
            it->second.session->state() == SessionState::Closed) {
            return false;
        }

        const auto desired = epollEventsFor(it->second.session->interest());
        if (desired == it->second.registered_events) {
            return true;
        }

        epoll_event event{};
        event.events = desired;
        event.data.u64 = it->second.session->token().generation;
        if (::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) != 0) {
            return false;
        }
        it->second.registered_events = desired;
        return true;
    }

    void closeSession(
        const int fd,
        const SessionCloseReason reason,
        const bool notify_release_observer = true
    ) noexcept {
        const auto it = sessions.find(fd);
        if (it == sessions.end()) {
            return;
        }

        auto& session = *it->second.session;
        if (session.state() != SessionState::Closing &&
            session.state() != SessionState::Closed) {
            (void)session.beginClose(reason);
        }
        const auto generation = session.token().generation;

        if (epoll_fd >= 0) {
            // DEL 先于 Timer 取消和 fd close，防止关闭中的 Session 再接收 I/O 事件。
            (void)::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
        }
        event_to_fd.erase(generation);
        cancelDeadline(it->second);
        const auto timers = session.releaseTimersForClose();
        cancelTimerId(timers.idle);
        cancelTimerId(timers.read);
        cancelTimerId(timers.write);
        cancelTimerId(timers.business);
        (void)session.finalizeClose();
        sessions.erase(it);
        if (notify_release_observer && release_connection) {
            release_connection();
        }
    }

    [[nodiscard]] bool dispatchRequest(
        const ConnectionToken token,
        std::vector<std::uint8_t> request
    ) noexcept {
        if (drain_control != nullptr &&
            drain_control->drainRequested()) {
            closeSession(token.fd, SessionCloseReason::LocalShutdown);
            return false;
        }
        runtime::WorkerSubmitStatus submit_status =
            runtime::WorkerSubmitStatus::Rejected;
        try {
            BusinessRequest business_request;
            business_request.token = token;
            business_request.request_payload = std::move(request);
            auto task = makeBusinessTask(
                std::move(business_request),
                business_handler,
                std::weak_ptr<CompletionRouter>(completion_router),
                config.max_completion_bytes -
                    protocol::kFrameHeaderSize
            );
            submit_status = worker_pool->trySubmit(std::move(task));
            if (submit_status == runtime::WorkerSubmitStatus::Accepted) {
                const auto it = sessions.find(token.fd);
                if (it != sessions.end()) {
                    it->second.business_expired = false;
                }
                if (!syncDeadline(token.fd)) {
                    closeSession(
                        token.fd,
                        SessionCloseReason::ResourceExhausted
                    );
                    return false;
                }
                return true;
            }
        } catch (...) {
        }


        if (drain_control != nullptr &&
            drain_control->drainRequested()) {
            closeSession(token.fd, SessionCloseReason::LocalShutdown);
            return false;
        }

        bool output_limit_exceeded = false;
        const auto it = sessions.find(token.fd);
        if (it != sessions.end() && it->second.session->matches(token) &&
            !config.overload_response_frame.empty()) {
            SessionStatus response_status = SessionStatus::InvalidState;
            try {
                response_status = it->second.session->queueResponse(
                    token,
                    config.overload_response_frame
                );
            } catch (...) {
                response_status = SessionStatus::OutputLimitExceeded;
            }
            if (response_status == SessionStatus::Ok) {
                if (!syncDeadline(token.fd)) {
                    closeSession(
                        token.fd,
                        SessionCloseReason::ResourceExhausted
                    );
                    return false;
                }
                handleWrite(token.fd);
                return false;
            }
            if (response_status == SessionStatus::OutputLimitExceeded) {
                output_limit_exceeded = true;
            }
        }

        closeSession(
            token.fd,
            output_limit_exceeded
                ? SessionCloseReason::OutputLimitExceeded
                : SessionCloseReason::BusinessQueueRejected
        );
        return false;
    }

    void handleRead(
        const int fd,
        const bool drain_peer_close
    ) noexcept {
        // 连接 fd 也是 ET，recv 循环必须持续到 EAGAIN 或状态关闭。
        while (sessions.find(fd) != sessions.end()) {
            const auto bytes = ::recv(
                fd,
                read_scratch.data(),
                read_scratch.size(),
                0
            );
            if (bytes > 0) {
                auto it = sessions.find(fd);
                if (it == sessions.end()) {
                    return;
                }
                SessionResult result;
                try {
                    result = it->second.session->onInput(
                        base::ArrayView<const std::uint8_t>(read_scratch).first(
                            static_cast<std::size_t>(bytes)
                        )
                    );
                } catch (...) {
                    closeSession(
                        fd,
                        SessionCloseReason::ResourceExhausted
                    );
                    return;
                }
                if (result.request.has_value()) {
                    const auto token = it->second.session->token();
                    if (!dispatchRequest(
                            token,
                            std::move(*result.request)
                        )) {
                        return;
                    }
                }
                it = sessions.find(fd);
                if (it == sessions.end()) {
                    return;
                }
                if (it->second.session->state() == SessionState::Closing) {
                    closeSession(fd, it->second.session->closeReason());
                    return;
                }
                if (it->second.session->state() == SessionState::Reading &&
                    !syncDeadline(fd)) {
                    closeSession(
                        fd,
                        SessionCloseReason::ResourceExhausted
                    );
                    return;
                }
                if (!drain_peer_close && !hasInterest(
                        it->second.session->interest(),
                        SessionInterest::Read
                    )) {
                    if (!modifyInterest(fd)) {
                        closeSession(fd, SessionCloseReason::IoError);
                    }
                    return;
                }
                continue;
            }

            if (bytes == 0) {
                auto it = sessions.find(fd);
                if (it == sessions.end()) {
                    return;
                }
                (void)it->second.session->onPeerReadClosed();
                if (it->second.session->state() == SessionState::Closing) {
                    closeSession(fd, it->second.session->closeReason());
                } else if (!modifyInterest(fd)) {
                    closeSession(fd, SessionCloseReason::IoError);
                }
                return;
            }

            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!modifyInterest(fd) &&
                    sessions.find(fd) != sessions.end()) {
                    closeSession(fd, SessionCloseReason::IoError);
                }
                return;
            }

            closeSession(fd, SessionCloseReason::IoError);
            return;
        }
    }

    void handleWrite(const int fd) noexcept {
        // send 可能短写；只在 EAGAIN 时保留 EPOLLOUT，可写时尽量排空当前响应。
        while (true) {
            auto it = sessions.find(fd);
            if (it == sessions.end()) {
                return;
            }
            if (it->second.session->state() != SessionState::Writing) {
                if (it->second.session->state() == SessionState::Closing) {
                    closeSession(fd, it->second.session->closeReason());
                } else if (!modifyInterest(fd)) {
                    closeSession(fd, SessionCloseReason::IoError);
                }
                return;
            }

            const auto output = it->second.session->pendingOutput();
            if (output.empty()) {
                closeSession(fd, SessionCloseReason::IoError);
                return;
            }

            const auto bytes = ::send(
                fd,
                output.data(),
                output.size(),
                MSG_NOSIGNAL
            );
            if (bytes > 0) {
                SessionResult progress;
                try {
                    progress = it->second.session->onBytesWritten(
                        static_cast<std::size_t>(bytes)
                    );
                } catch (...) {
                    closeSession(
                        fd,
                        SessionCloseReason::ResourceExhausted
                    );
                    return;
                }
                if (progress.request.has_value()) {
                    const auto token = it->second.session->token();
                    if (!dispatchRequest(
                            token,
                            std::move(*progress.request)
                        )) {
                        return;
                    }
                } else if (!syncDeadline(fd)) {
                    closeSession(
                        fd,
                        SessionCloseReason::ResourceExhausted
                    );
                    return;
                }
                continue;
            }

            if (bytes == 0) {
                closeSession(fd, SessionCloseReason::IoError);
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!modifyInterest(fd)) {
                    closeSession(fd, SessionCloseReason::IoError);
                }
                return;
            }

            closeSession(fd, SessionCloseReason::IoError);
            return;
        }
    }

    void applyCompletion(BusinessCompletion completion) noexcept {
        // loop_id/fd/generation 三元组匹配后才能把 Worker 结果交回 Session。
        auto it = sessions.find(completion.token.fd);
        if (it == sessions.end() ||
            !it->second.session->matches(completion.token)) {
            return;
        }

        if (it->second.business_expired ||
            (config.deadlines.enabled && it->second.deadline_active &&
             it->second.deadline_kind ==
                 timer::TimerEventKind::BusinessTimeout &&
             timer::SteadyClock::now() >= it->second.deadline_at)) {
            if (!it->second.business_expired) {
                handleTimeoutEvent(
                    sessionTimerEvent(
                        timer::TimerEventKind::BusinessTimeout,
                        completion.token,
                        it->second.deadline_sequence
                    )
                );
                it = sessions.find(completion.token.fd);
            }
            return;
        }

        cancelDeadline(it->second);
        if (completion.status != BusinessCompletionStatus::Response) {
            SessionCloseReason reason = SessionCloseReason::BusinessError;
            if (completion.status ==
                BusinessCompletionStatus::InvalidRequest) {
                reason = SessionCloseReason::ProtocolError;
            } else if (completion.status ==
                       BusinessCompletionStatus::OutputLimitExceeded) {
                reason = SessionCloseReason::OutputLimitExceeded;
            }
            closeSession(completion.token.fd, reason);
            return;
        }

        SessionStatus status = SessionStatus::InvalidState;
        try {
            status = it->second.session->queueResponse(
                completion.token,
                completion.response_frame
            );
        } catch (...) {
            closeSession(
                completion.token.fd,
                SessionCloseReason::ResourceExhausted
            );
            return;
        }
        if (status != SessionStatus::Ok) {
            if (sessions.find(completion.token.fd) != sessions.end()) {
                closeSession(
                    completion.token.fd,
                    status == SessionStatus::OutputLimitExceeded
                        ? SessionCloseReason::OutputLimitExceeded
                        : SessionCloseReason::BusinessError
                );
            }
            return;
        }

        if (drain_control != nullptr &&
            drain_control->drainRequested()) {
            (void)it->second.session->closeAfterWrite(
                SessionCloseReason::LocalShutdown
            );
        }
        if (!syncDeadline(completion.token.fd)) {
            closeSession(
                completion.token.fd,
                SessionCloseReason::ResourceExhausted
            );
            return;
        }
        handleWrite(completion.token.fd);
    }

    void applyGracefulDrain() noexcept {
        if (drain_control == nullptr ||
            !drain_control->drainRequested()) {
            return;
        }

        std::vector<int> descriptors;
        descriptors.reserve(sessions.size());
        for (const auto& [fd, entry] : sessions) {
            const auto state = entry.session->state();
            if (state == SessionState::Reading ||
                state == SessionState::Writing) {
                descriptors.push_back(fd);
            }
        }

        for (const int fd : descriptors) {
            const auto it = sessions.find(fd);
            if (it == sessions.end()) {
                continue;
            }
            if (it->second.session->state() == SessionState::Reading) {
                closeSession(fd, SessionCloseReason::LocalShutdown);
                continue;
            }
            if (it->second.session->state() == SessionState::Writing) {
                (void)it->second.session->closeAfterWrite(
                    SessionCloseReason::LocalShutdown
                );
                handleWrite(fd);
            }
        }
    }

    void handleTimeoutEvent(const timer::TimerEvent event) noexcept {
        const auto it = sessions.find(event.target_fd);
        if (it == sessions.end()) {
            return;
        }
        const auto token = it->second.session->token();
        // sequence 与 token 共同过滤取消竞态中的旧 Timer，只处理当前状态所对应的 deadline。
        if (event.target_loop_id != token.loop_id ||
            event.target_fd != token.fd ||
            event.target_generation != token.generation ||
            !it->second.deadline_active ||
            event.target_sequence != it->second.deadline_sequence ||
            event.kind != it->second.deadline_kind) {
            return;
        }

        const auto expected_kind = deadlineKindFor(*it->second.session);
        if (event.kind != expected_kind ||
            timer::SteadyClock::now() < it->second.deadline_at) {
            return;
        }

        (void)it->second.session->replaceTimer(
            sessionTimerKind(event.kind),
            {}
        );
        it->second.deadline_active = false;
        switch (event.kind) {
            case timer::TimerEventKind::IdleTimeout:
                closeSession(event.target_fd, SessionCloseReason::IdleTimeout);
                return;
            case timer::TimerEventKind::ReadTimeout:
                closeSession(event.target_fd, SessionCloseReason::ReadTimeout);
                return;
            case timer::TimerEventKind::WriteTimeout:
                closeSession(event.target_fd, SessionCloseReason::WriteTimeout);
                return;
            case timer::TimerEventKind::BusinessTimeout:
                it->second.business_expired = true;
                if (!config.business_timeout_response_frame.empty()) {
                    SessionStatus status = SessionStatus::InvalidState;
                    try {
                        status = it->second.session->queueResponse(
                            token,
                            config.business_timeout_response_frame
                        );
                    } catch (...) {
                        status = SessionStatus::OutputLimitExceeded;
                    }
                    if (status == SessionStatus::Ok) {
                        if (it->second.session->closeAfterWrite(
                                SessionCloseReason::BusinessTimeout
                            ) != SessionStatus::Ok) {
                            closeSession(
                                event.target_fd,
                                SessionCloseReason::BusinessTimeout
                            );
                            return;
                        }
                        if (!syncDeadline(event.target_fd)) {
                            closeSession(
                                event.target_fd,
                                SessionCloseReason::ResourceExhausted
                            );
                            return;
                        }
                        handleWrite(event.target_fd);
                        return;
                    }
                }
                closeSession(
                    event.target_fd,
                    SessionCloseReason::BusinessTimeout
                );
                return;
            case timer::TimerEventKind::CleanupTick:
            case timer::TimerEventKind::BlacklistMaintenanceTick:
                return;
        }
    }

    [[nodiscard]] bool drainTimeoutMailbox() noexcept {
        if (timeout_mailbox == nullptr) {
            return true;
        }
        if (!timeout_mailbox->drainWakeSignal()) {
            return false;
        }
        while (auto event = timeout_mailbox->tryPop()) {
            handleTimeoutEvent(*event);
        }
        if (timeout_mailbox->takeRescanRequest()) {
            std::vector<timer::TimerEvent> expired;
            try {
                expired.reserve(sessions.size());
                const auto now = timer::SteadyClock::now();
                for (const auto& [unused_fd, entry] : sessions) {
                    static_cast<void>(unused_fd);
                    if (!entry.deadline_active || entry.deadline_at > now) {
                        continue;
                    }
                    const auto token = entry.session->token();
                    expired.push_back(sessionTimerEvent(
                        entry.deadline_kind,
                        token,
                        entry.deadline_sequence
                    ));
                }
            } catch (...) {
                return false;
            }
            for (const auto& event : expired) {
                handleTimeoutEvent(event);
            }
        }
        return true;
    }

    [[nodiscard]] bool drainMailboxes() noexcept {
        if (!completion_mailbox->drainWakeSignal()) {
            return false;
        }

        while (auto completion = completion_mailbox->tryPop()) {
            applyCompletion(std::move(*completion));
        }

        if (!drainTimeoutMailbox()) {
            return false;
        }

        if (connection_mailbox == nullptr) {
            return true;
        }
        if (!connection_mailbox->drainWakeSignal()) {
            return false;
        }
        while (auto socket = connection_mailbox->tryPop()) {
            const auto status = adopt(std::move(*socket));
            if (status != EventLoopStatus::Ok &&
                release_connection) {
                release_connection();
            }
        }
        applyGracefulDrain();
        return true;
    }

    void handleSocketEvent(
        const std::uint64_t generation,
        const std::uint32_t event_mask
    ) noexcept {
        const auto event_it = event_to_fd.find(generation);
        if (event_it == event_to_fd.end()) {
            return;
        }
        const int fd = event_it->second;

        const bool has_error = (event_mask & EPOLLERR) != 0U;
        const bool has_hangup = (event_mask & EPOLLHUP) != 0U;
        if (has_error || has_hangup) {
            int socket_error = 0;
            socklen_t error_length = sizeof(socket_error);
            int query_result = -1;
            do {
                query_result = ::getsockopt(
                    fd,
                    SOL_SOCKET,
                    SO_ERROR,
                    &socket_error,
                    &error_length
                );
            } while (query_result != 0 && errno == EINTR);
            closeSession(
                fd,
                has_error || query_result != 0 || socket_error != 0
                    ? SessionCloseReason::IoError
                    : SessionCloseReason::PeerClosed
            );
            return;
        }

        const auto session_it = sessions.find(fd);
        const bool read_interested =
            session_it != sessions.end() &&
            hasInterest(
                session_it->second.session->interest(),
                SessionInterest::Read
            );
        if ((event_mask & EPOLLRDHUP) != 0U ||
            ((event_mask & EPOLLIN) != 0U && read_interested)) {
            handleRead(
                fd,
                (event_mask & EPOLLRDHUP) != 0U
            );
        }
        if (sessions.find(fd) == sessions.end()) {
            return;
        }
        if ((event_mask & EPOLLOUT) != 0U) {
            handleWrite(fd);
        }
    }

    [[nodiscard]] EventLoopStatus pollOnce(const int timeout_ms) noexcept {
        if (!onOwnerThread()) {
            return EventLoopStatus::WrongThread;
        }
        if (stopped || epoll_fd < 0) {
            return EventLoopStatus::Stopped;
        }
        if (timeout_ms < -1) {
            return EventLoopStatus::InvalidArgument;
        }

        int ready = -1;
        do {
            ready = ::epoll_wait(
                epoll_fd,
                events.data(),
                static_cast<int>(events.size()),
                timeout_ms
            );
        } while (ready < 0 && errno == EINTR);

        if (ready < 0) {
            return EventLoopStatus::SystemCallFailed;
        }

        for (int index = 0; index < ready; ++index) {
            const auto& event = events[static_cast<std::size_t>(index)];
            if (event.data.u64 == 0) {
                if (!drainMailboxes()) {
                    return EventLoopStatus::SystemCallFailed;
                }
                continue;
            }
            handleSocketEvent(event.data.u64, event.events);
        }
        return EventLoopStatus::Ok;
    }

    void forceShutdown() noexcept {
        if (stopped && epoll_fd < 0) {
            return;
        }
        unbindCompletionRouter();
        completion_mailbox->close();
        if (timeout_mailbox != nullptr) {
            timeout_mailbox->close();
        }
        if (connection_mailbox != nullptr) {
            const auto discarded = connection_mailbox->close();
            if (release_connection) {
                for (std::size_t index = 0; index < discarded; ++index) {
                    release_connection();
                }
            }
        }
        while (!sessions.empty()) {
            closeSession(
                sessions.begin()->first,
                SessionCloseReason::LocalShutdown
            );
        }
        if (epoll_fd >= 0) {
            (void)::close(epoll_fd);
            epoll_fd = -1;
        }
        stopped = true;
    }

    [[nodiscard]] EventLoopStatus shutdown() noexcept {
        if (!onOwnerThread()) {
            return EventLoopStatus::WrongThread;
        }
        forceShutdown();
        return EventLoopStatus::Ok;
    }

    EventLoopConfig config;
    runtime::BoundedWorkerPool* worker_pool = nullptr;
    std::shared_ptr<IFrameBusinessHandler> business_handler;
    std::shared_ptr<ConnectionMailbox> connection_mailbox;
    std::function<void()> release_connection;
    std::shared_ptr<LoopMailbox> completion_mailbox;
    std::shared_ptr<TimeoutMailbox> timeout_mailbox;
    std::shared_ptr<CompletionRouter> completion_router;
    std::shared_ptr<EventLoopDrainControl> drain_control;
    timer::ITimerScheduler* timer_scheduler = nullptr;
    std::vector<epoll_event> events;
    std::vector<std::uint8_t> read_scratch;
    std::thread::id owner_thread;
    std::pmr::unsynchronized_pool_resource session_resource;
    SessionPool session_pool{&session_resource};
    std::unordered_map<int, SessionEntry> sessions;
    std::unordered_map<std::uint64_t, int> event_to_fd;
    std::uint64_t next_generation = 1;
    int epoll_fd = -1;
    bool router_bound = false;
    bool stopped = false;
};

EventLoop::EventLoop(
    EventLoopConfig config,
    runtime::BoundedWorkerPool& worker_pool,
    std::shared_ptr<IFrameBusinessHandler> business_handler,
    timer::ITimerScheduler& timer_scheduler,
    std::shared_ptr<ConnectionMailbox> connection_mailbox,
    std::function<void()> release_connection,
    std::shared_ptr<CompletionRouter> completion_router,
    std::shared_ptr<EventLoopDrainControl> drain_control
) : impl_(std::make_unique<Impl>(
        std::move(config),
        worker_pool,
        std::move(business_handler),
        std::move(connection_mailbox),
        std::move(release_connection),
        std::move(completion_router),
        std::move(drain_control),
        &timer_scheduler
    )) {}

EventLoop::~EventLoop() = default;

EventLoopStatus EventLoop::adopt(OwnedSocket socket) noexcept {
    return impl_->adopt(std::move(socket));
}

EventLoopStatus EventLoop::pollOnce(const int timeout_ms) noexcept {
    return impl_->pollOnce(timeout_ms);
}

EventLoopStatus EventLoop::shutdown() noexcept {
    return impl_->shutdown();
}

}  // 命名空间 aegisflow::net
