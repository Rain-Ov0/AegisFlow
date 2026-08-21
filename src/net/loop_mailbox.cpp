#include "aegisflow/net/loop_mailbox.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace aegisflow::net {

class LoopMailbox::Impl final {
public:
    Impl(
        const std::size_t capacity,
        const std::size_t byte_capacity,
        const std::size_t max_completion_bytes
    )
        : queue(capacity) {
        if (capacity == 0 || byte_capacity == 0 ||
            max_completion_bytes == 0 ||
            max_completion_bytes > byte_capacity) {
            throw std::invalid_argument("LoopMailbox 容量必须大于零");
        }

        wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake_fd < 0) {
            throw std::system_error(
                errno,
                std::generic_category(),
                "创建 LoopMailbox eventfd 失败"
            );
        }
        this->byte_capacity = byte_capacity;
        this->max_completion_bytes = max_completion_bytes;
    }

    ~Impl() {
        if (wake_fd >= 0) {
            (void)::close(wake_fd);
            wake_fd = -1;
        }
    }

    mutable std::mutex mutex;
    std::vector<std::optional<BusinessCompletion>> queue;
    std::size_t head = 0;
    std::size_t tail = 0;
    std::size_t size = 0;
    int wake_fd = -1;
    std::size_t buffered_bytes = 0;
    std::size_t byte_capacity = 0;
    std::size_t max_completion_bytes = 0;
    bool accepting = true;
};

LoopMailbox::LoopMailbox(
    const std::size_t capacity,
    const std::size_t byte_capacity,
    const std::size_t max_completion_bytes
) : impl_(std::make_unique<Impl>(
        capacity,
        byte_capacity,
        max_completion_bytes
    )) {}

LoopMailbox::~LoopMailbox() = default;

bool LoopMailbox::tryPost(
    BusinessCompletion completion
) noexcept {
    std::lock_guard lock(impl_->mutex);
    const bool response_shape_valid =
        completion.status == BusinessCompletionStatus::Response
            ? !completion.response_frame.empty()
            : completion.response_frame.empty();
    if (!completion.token.valid() || !response_shape_valid) {
        return false;
    }
    const auto completion_bytes = completion.response_frame.size();
    if (completion_bytes > impl_->max_completion_bytes) {
        return false;
    }
    if (!impl_->accepting) {
        return false;
    }
    if (impl_->size == impl_->queue.size()) {
        return false;
    }
    if (completion_bytes >
        impl_->byte_capacity - impl_->buffered_bytes) {
        return false;
    }

    const auto inserted_index = impl_->tail;
    // 先入有界队列再写 eventfd；真实唤醒失败时回滚槽位和字节计数。
    impl_->queue[inserted_index] = std::move(completion);
    impl_->tail = (impl_->tail + 1) % impl_->queue.size();
    ++impl_->size;
    impl_->buffered_bytes += completion_bytes;

    const std::uint64_t increment = 1;
    while (true) {
        const auto written = ::write(
            impl_->wake_fd,
            &increment,
            sizeof(increment)
        );
        if (written == static_cast<ssize_t>(sizeof(increment))) {
            break;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && errno == EAGAIN) {
            break;
        }

        impl_->queue[inserted_index].reset();
        impl_->tail = inserted_index;
        --impl_->size;
        impl_->buffered_bytes -= completion_bytes;
        return false;
    }

    return true;
}

std::optional<BusinessCompletion> LoopMailbox::tryPop() noexcept {
    std::lock_guard lock(impl_->mutex);
    if (impl_->size == 0) {
        return std::nullopt;
    }

    const auto completion_bytes =
        impl_->queue[impl_->head]->response_frame.size();
    auto completion = std::move(impl_->queue[impl_->head]);
    impl_->queue[impl_->head].reset();
    impl_->head = (impl_->head + 1) % impl_->queue.size();
    --impl_->size;
    impl_->buffered_bytes -= completion_bytes;
    return completion;
}

bool LoopMailbox::drainWakeSignal() noexcept {
    while (true) {
        std::uint64_t value = 0;
        const auto bytes = ::read(impl_->wake_fd, &value, sizeof(value));
        if (bytes == static_cast<ssize_t>(sizeof(value))) {
            continue;
        }
        if (bytes < 0 && errno == EINTR) {
            continue;
        }
        if (bytes < 0 && errno == EAGAIN) {
            return true;
        }

        return false;
    }
}

void LoopMailbox::close() noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->accepting = false;
    while (impl_->size != 0) {
        impl_->buffered_bytes -=
            impl_->queue[impl_->head]->response_frame.size();
        impl_->queue[impl_->head].reset();
        impl_->head = (impl_->head + 1) % impl_->queue.size();
        --impl_->size;
    }
    impl_->tail = impl_->head;
    impl_->buffered_bytes = 0;
}

int LoopMailbox::wakeFd() const noexcept {
    return impl_->wake_fd;
}

namespace {

[[nodiscard]] bool signalWakeFd(const int wake_fd) noexcept {
    const std::uint64_t increment = 1;
    while (true) {
        const auto written = ::write(wake_fd, &increment, sizeof(increment));
        if (written == static_cast<ssize_t>(sizeof(increment))) {
            return true;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return written < 0 && errno == EAGAIN;
    }
}

bool drainWakeFd(const int wake_fd) noexcept {
    while (true) {
        std::uint64_t value = 0;
        const auto bytes = ::read(wake_fd, &value, sizeof(value));
        if (bytes == static_cast<ssize_t>(sizeof(value))) {
            continue;
        }
        if (bytes < 0 && errno == EINTR) {
            continue;
        }
        if (bytes < 0 && errno == EAGAIN) {
            return true;
        }
        return false;
    }
}

[[nodiscard]] bool connectionTimeoutEvent(
    const timer::TimerEvent& event
) noexcept {
    const bool valid_kind =
        event.kind == timer::TimerEventKind::IdleTimeout ||
        event.kind == timer::TimerEventKind::ReadTimeout ||
        event.kind == timer::TimerEventKind::WriteTimeout ||
        event.kind == timer::TimerEventKind::BusinessTimeout;
    return valid_kind && event.target_fd >= 0 &&
           event.target_generation != 0 && event.target_sequence != 0;
}

}  // 命名空间

class ConnectionMailbox::Impl final {
public:
    explicit Impl(const std::size_t capacity) : queue(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("ConnectionMailbox 容量必须大于零");
        }
        wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake_fd < 0) {
            throw std::system_error(
                errno, std::generic_category(),
                "创建 ConnectionMailbox eventfd 失败");
        }
    }

    ~Impl() {
        if (wake_fd >= 0) {
            (void)::close(wake_fd);
        }
    }

    mutable std::mutex mutex;
    std::vector<std::optional<OwnedSocket>> queue;
    std::size_t head = 0;
    std::size_t tail = 0;
    std::size_t size = 0;
    int wake_fd = -1;
    bool accepting = true;
};

ConnectionMailbox::ConnectionMailbox(const std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)) {}

ConnectionMailbox::~ConnectionMailbox() = default;

bool ConnectionMailbox::tryPost(
    OwnedSocket socket
) noexcept {
    std::lock_guard lock(impl_->mutex);
    if (!socket.valid()) {
        return false;
    }
    if (!impl_->accepting) {
        return false;
    }
    if (impl_->size == impl_->queue.size()) {
        return false;
    }

    const auto index = impl_->tail;
    impl_->queue[index] = std::move(socket);
    impl_->tail = (impl_->tail + 1) % impl_->queue.size();
    ++impl_->size;
    if (!signalWakeFd(impl_->wake_fd)) {
        impl_->queue[index].reset();
        impl_->tail = index;
        --impl_->size;
        return false;
    }
    return true;
}

std::optional<OwnedSocket> ConnectionMailbox::tryPop() noexcept {
    std::lock_guard lock(impl_->mutex);
    if (impl_->size == 0) {
        return std::nullopt;
    }
    auto socket = std::move(impl_->queue[impl_->head]);
    impl_->queue[impl_->head].reset();
    impl_->head = (impl_->head + 1) % impl_->queue.size();
    --impl_->size;
    return socket;
}

bool ConnectionMailbox::drainWakeSignal() noexcept {
    std::lock_guard lock(impl_->mutex);
    return drainWakeFd(impl_->wake_fd);
}

std::size_t ConnectionMailbox::close() noexcept {
    std::size_t discarded = 0;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->accepting = false;
        while (impl_->size != 0) {
            impl_->queue[impl_->head].reset();
            impl_->head = (impl_->head + 1) % impl_->queue.size();
            --impl_->size;
            ++discarded;
        }
        impl_->tail = impl_->head;
    }
    (void)signalWakeFd(impl_->wake_fd);
    return discarded;
}

int ConnectionMailbox::wakeFd() const noexcept { return impl_->wake_fd; }

class TimeoutMailbox::Impl final {
public:
    explicit Impl(const std::size_t capacity) : queue(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("TimeoutMailbox 容量必须大于零");
        }
        wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake_fd < 0) {
            throw std::system_error(
                errno, std::generic_category(),
                "创建 TimeoutMailbox eventfd 失败");
        }
    }

    ~Impl() {
        if (wake_fd >= 0) {
            (void)::close(wake_fd);
        }
    }

    mutable std::mutex mutex;
    std::vector<std::optional<timer::TimerEvent>> queue;
    std::size_t head = 0;
    std::size_t tail = 0;
    std::size_t size = 0;
    int wake_fd = -1;
    bool accepting = true;
    bool rescan_required = false;
};

TimeoutMailbox::TimeoutMailbox(const std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)) {}

TimeoutMailbox::~TimeoutMailbox() = default;

bool TimeoutMailbox::tryPost(const timer::TimerEvent event) noexcept {
    std::lock_guard lock(impl_->mutex);
    if (!connectionTimeoutEvent(event)) {
        return false;
    }
    if (!impl_->accepting) {
        return false;
    }
    if (impl_->size == impl_->queue.size()) {
        // Timer 邮箱饱和时请求 loop 全量扫描 deadline，避免超时事件丢失。
        impl_->rescan_required = true;
        if (!signalWakeFd(impl_->wake_fd)) {
            return false;
        }
        return true;
    }

    const auto index = impl_->tail;
    impl_->queue[index] = event;
    impl_->tail = (impl_->tail + 1) % impl_->queue.size();
    ++impl_->size;
    if (!signalWakeFd(impl_->wake_fd)) {
        impl_->queue[index].reset();
        impl_->tail = index;
        --impl_->size;
        return false;
    }
    return true;
}

std::optional<timer::TimerEvent> TimeoutMailbox::tryPop() noexcept {
    std::lock_guard lock(impl_->mutex);
    if (impl_->size == 0) {
        return std::nullopt;
    }
    auto event = std::move(impl_->queue[impl_->head]);
    impl_->queue[impl_->head].reset();
    impl_->head = (impl_->head + 1) % impl_->queue.size();
    --impl_->size;
    return event;
}

bool TimeoutMailbox::drainWakeSignal() noexcept {
    std::lock_guard lock(impl_->mutex);
    return drainWakeFd(impl_->wake_fd);
}

bool TimeoutMailbox::takeRescanRequest() noexcept {
    std::lock_guard lock(impl_->mutex);
    const bool requested = impl_->rescan_required;
    impl_->rescan_required = false;
    return requested;
}

void TimeoutMailbox::close() noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->accepting = false;
    impl_->rescan_required = false;
    while (impl_->size != 0) {
        impl_->queue[impl_->head].reset();
        impl_->head = (impl_->head + 1) % impl_->queue.size();
        --impl_->size;
    }
    impl_->tail = impl_->head;
}

int TimeoutMailbox::wakeFd() const noexcept { return impl_->wake_fd; }

}  // 命名空间 aegisflow::net
