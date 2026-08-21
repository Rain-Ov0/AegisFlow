#include "aegisflow/net/acceptor_loop.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace aegisflow::net {

namespace {

constexpr std::uint64_t kListenEvent = 1;
constexpr std::uint64_t kStopEvent = 2;

enum class AcceptorLoopState {
    Constructed,
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed,
};

[[nodiscard]] sockaddr_in checkedAddress(
    const AcceptorLoopConfig& config
) {
    if (config.bind_address.empty() || config.backlog <= 0 ||
        config.max_connections == 0 || config.max_events == 0 ||
        config.max_events >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("AcceptorLoop 配置无效");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config.port);
    if (::inet_pton(
            AF_INET,
            config.bind_address.c_str(),
            &address.sin_addr
        ) != 1) {
        throw std::invalid_argument("AcceptorLoop 仅接受合法 IPv4 绑定地址");
    }
    return address;
}

[[nodiscard]] bool signalEventFd(const int fd) noexcept {
    constexpr std::uint64_t increment = 1;
    while (true) {
        const auto written = ::write(fd, &increment, sizeof(increment));
        if (written == static_cast<ssize_t>(sizeof(increment))) {
            return true;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && errno == EAGAIN) {
            return true;
        }
        return false;
    }
}

[[nodiscard]] bool drainEventFd(const int fd) noexcept {
    while (true) {
        std::uint64_t value = 0;
        const auto bytes = ::read(fd, &value, sizeof(value));
        if (bytes == static_cast<ssize_t>(sizeof(value))) {
            continue;
        }
        if (bytes < 0 && errno == EINTR) {
            continue;
        }
        return bytes < 0 && errno == EAGAIN;
    }
}

[[nodiscard]] OwnedSocket openReserveFd() noexcept {
    return OwnedSocket(::open("/dev/null", O_RDONLY | O_CLOEXEC));
}

[[nodiscard]] bool isPendingNetworkError(const int error) noexcept {
    switch (error) {
        case EPROTO:
        case ENETDOWN:
        case ENOPROTOOPT:
        case EHOSTDOWN:
        case ENONET:
        case EHOSTUNREACH:
        case EOPNOTSUPP:
        case ENETUNREACH:
            return true;
        default:
            return false;
    }
}

}  // 命名空间

class AcceptorLoop::Impl final {
public:
    Impl(
        AcceptorLoopConfig acceptor_config,
        EventLoopGroup& event_loop_group
    ) : config(std::move(acceptor_config)),
        bind_address(checkedAddress(config)),
        event_loops(&event_loop_group),
        events(config.max_events) {}

    ~Impl() {
        stop();
        (void)join();
    }

    struct StartResources {
        OwnedSocket listener;
        OwnedSocket epoll;
        OwnedSocket wake;
        OwnedSocket reserve;
    };

    [[nodiscard]] AcceptorLoopStatus createResources(
        StartResources& resources
    ) noexcept {
        resources.listener.reset(::socket(
            AF_INET,
            SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
            0
        ));
        if (!resources.listener.valid()) {
            return AcceptorLoopStatus::SystemCallFailed;
        }
        constexpr int enabled = 1;
        if (::setsockopt(
                resources.listener.get(),
                SOL_SOCKET,
                SO_REUSEADDR,
                &enabled,
                sizeof(enabled)
            ) != 0) {
            return AcceptorLoopStatus::SystemCallFailed;
        }
        if (::bind(
                resources.listener.get(),
                reinterpret_cast<const sockaddr*>(&bind_address),
                sizeof(bind_address)
            ) != 0) {
            return errno == EADDRINUSE
                       ? AcceptorLoopStatus::AddressInUse
                       : AcceptorLoopStatus::SystemCallFailed;
        }
        resources.epoll.reset(::epoll_create1(EPOLL_CLOEXEC));
        if (!resources.epoll.valid()) {
            return AcceptorLoopStatus::SystemCallFailed;
        }
        resources.wake.reset(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
        if (!resources.wake.valid()) {
            return AcceptorLoopStatus::SystemCallFailed;
        }
        resources.reserve = openReserveFd();
        if (!resources.reserve.valid()) {
            return AcceptorLoopStatus::SystemCallFailed;
        }
        epoll_event listen_event{};
        listen_event.events = EPOLLIN | EPOLLET;
        listen_event.data.u64 = kListenEvent;
        if (::epoll_ctl(
                resources.epoll.get(),
                EPOLL_CTL_ADD,
                resources.listener.get(),
                &listen_event
            ) != 0) {
            return AcceptorLoopStatus::SystemCallFailed;
        }
        epoll_event stop_event{};
        stop_event.events = EPOLLIN | EPOLLET;
        stop_event.data.u64 = kStopEvent;
        if (::epoll_ctl(
                resources.epoll.get(),
                EPOLL_CTL_ADD,
                resources.wake.get(),
                &stop_event
            ) != 0) {
            return AcceptorLoopStatus::SystemCallFailed;
        }
        if (::listen(resources.listener.get(), config.backlog) != 0) {
            return AcceptorLoopStatus::SystemCallFailed;
        }
        return AcceptorLoopStatus::Ok;
    }

    void dispatchAccepted(OwnedSocket socket) noexcept {
        std::lock_guard lock(state_mutex);
        if (state != AcceptorLoopState::Running ||
            stop_requested.load(std::memory_order_acquire)) {
            return;
        }
        event_loops->tryDispatch(std::move(socket));
    }

    [[nodiscard]] bool recoverFromFdExhaustion() noexcept {
        // 预留 fd 让 EMFILE/ENFILE 时仍能 accept 并主动丢弃一条连接，再恢复预留槽位。
        reserve_fd.reset();
        while (!stop_requested.load(std::memory_order_acquire)) {
            OwnedSocket shed(::accept4(
                listen_fd.get(),
                nullptr,
                nullptr,
                SOCK_NONBLOCK | SOCK_CLOEXEC
            ));
            if (shed.valid()) {
                break;
            }
            const int error = errno;
            if (error == EINTR) {
                continue;
            }
            break;
        }

        reserve_fd = openReserveFd();
        return reserve_fd.valid();
    }

    void drainAccept() noexcept {
        // 监听 fd 使用 ET，一次通知必须 accept 到 EAGAIN，否则剩余连接可能不再触发边沿。
        while (!stop_requested.load(std::memory_order_acquire)) {
            OwnedSocket accepted(::accept4(
                listen_fd.get(),
                nullptr,
                nullptr,
                SOCK_NONBLOCK | SOCK_CLOEXEC
            ));
            if (accepted.valid()) {
                dispatchAccepted(std::move(accepted));
                continue;
            }

            const int error = errno;
            if (error == EINTR) {
                continue;
            }
            if (error == EAGAIN || error == EWOULDBLOCK) {
                return;
            }
            if (error == ECONNABORTED) {
                continue;
            }
            if (error == EMFILE || error == ENFILE) {
                if (!recoverFromFdExhaustion()) {
                    return;
                }
                continue;
            }
            if (isPendingNetworkError(error)) {
                continue;
            }
            return;
        }
    }

    void releaseResources() noexcept {
        // listen fd 最先释放，保证 join 返回前端口已停止接入。
        listen_fd.reset();
        reserve_fd.reset();
        wake_fd.reset();
        epoll_fd.reset();
    }

    void threadMain() noexcept {
        {
            std::unique_lock lock(state_mutex);
            thread_id = std::this_thread::get_id();
            state_changed.notify_all();
            state_changed.wait(lock, [this] {
                return state != AcceptorLoopState::Starting;
            });
        }

        bool failed = false;
        while (!stop_requested.load(std::memory_order_acquire)) {
            const int ready = ::epoll_wait(
                epoll_fd.get(),
                events.data(),
                static_cast<int>(events.size()),
                -1
            );
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                failed = true;
                break;
            }

            for (int index = 0; index < ready; ++index) {
                if (stop_requested.load(std::memory_order_acquire)) {
                    break;
                }
                const auto tag = events[static_cast<std::size_t>(index)]
                                     .data.u64;
                if (tag == kStopEvent) {
                    if (!drainEventFd(wake_fd.get())) {
                        failed = true;
                    }
                    break;
                }
                if (tag == kListenEvent) {
                    drainAccept();
                }
            }
        }

        std::lock_guard lock(state_mutex);
        releaseResources();
        if (failed) {
            state = AcceptorLoopState::Failed;
            join_status = AcceptorLoopStatus::SystemCallFailed;
        } else if (state != AcceptorLoopState::Failed) {
            state = AcceptorLoopState::Stopped;
        }
        state_changed.notify_all();
    }

    [[nodiscard]] AcceptorLoopStatus start() noexcept {
        std::unique_lock lock(state_mutex);
        if (state == AcceptorLoopState::Running) {
            return AcceptorLoopStatus::Ok;
        }
        if (state != AcceptorLoopState::Constructed) {
            return AcceptorLoopStatus::InvalidState;
        }
        state = AcceptorLoopState::Starting;

        StartResources resources;
        const auto resource_status = createResources(resources);
        if (resource_status != AcceptorLoopStatus::Ok) {
            state = AcceptorLoopState::Failed;
            join_status = resource_status;
            return resource_status;
        }

        listen_fd = std::move(resources.listener);
        epoll_fd = std::move(resources.epoll);
        wake_fd = std::move(resources.wake);
        reserve_fd = std::move(resources.reserve);
        stop_requested.store(false, std::memory_order_release);
        try {
            thread = std::thread([this] { threadMain(); });
        } catch (...) {
            releaseResources();
            state = AcceptorLoopState::Failed;
            join_status = AcceptorLoopStatus::StartFailed;
            return join_status;
        }

        state_changed.wait(lock, [this] {
            return thread_id != std::thread::id{};
        });
        state = AcceptorLoopState::Running;
        state_changed.notify_all();
        return AcceptorLoopStatus::Ok;
    }

    void stop() noexcept {
        std::lock_guard lock(state_mutex);
        if (state == AcceptorLoopState::Constructed) {
            stop_requested.store(true, std::memory_order_release);
            state = AcceptorLoopState::Stopped;
            return;
        }
        if (state != AcceptorLoopState::Running) {
            return;
        }
        stop_requested.store(true, std::memory_order_release);
        state = AcceptorLoopState::Stopping;
        if (wake_fd.valid()) {
            (void)signalEventFd(wake_fd.get());
        }
    }

    [[nodiscard]] AcceptorLoopStatus join() noexcept {
        std::thread joining_thread;
        {
            std::unique_lock lock(state_mutex);
            if (thread_id == std::this_thread::get_id()) {
                return AcceptorLoopStatus::SelfJoin;
            }
            if (state == AcceptorLoopState::Starting ||
                state == AcceptorLoopState::Running) {
                return AcceptorLoopStatus::InvalidState;
            }
            if (join_in_progress) {
                state_changed.wait(lock, [this] {
                    return !join_in_progress;
                });
                return join_status;
            }
            if (!thread.joinable()) {
                return join_status;
            }
            joining_thread = std::move(thread);
            join_in_progress = true;
        }

        joining_thread.join();

        std::lock_guard lock(state_mutex);
        join_in_progress = false;
        state_changed.notify_all();
        return join_status;
    }

    AcceptorLoopConfig config;
    sockaddr_in bind_address{};
    EventLoopGroup* event_loops = nullptr;
    std::vector<epoll_event> events;
    mutable std::mutex state_mutex;
    std::condition_variable state_changed;
    OwnedSocket listen_fd;
    OwnedSocket epoll_fd;
    OwnedSocket wake_fd;
    OwnedSocket reserve_fd;
    std::thread thread;
    std::thread::id thread_id;
    std::atomic<bool> stop_requested{false};
    AcceptorLoopState state = AcceptorLoopState::Constructed;
    AcceptorLoopStatus join_status = AcceptorLoopStatus::Ok;
    bool join_in_progress = false;
};

AcceptorLoop::AcceptorLoop(
    AcceptorLoopConfig config,
    EventLoopGroup& event_loops
) : impl_(std::make_unique<Impl>(
        std::move(config),
        event_loops
    )) {}

AcceptorLoop::~AcceptorLoop() = default;

AcceptorLoopStatus AcceptorLoop::start() noexcept {
    return impl_->start();
}

void AcceptorLoop::stop() noexcept {
    impl_->stop();
}

AcceptorLoopStatus AcceptorLoop::join() noexcept {
    return impl_->join();
}

}  // 命名空间 aegisflow::net
