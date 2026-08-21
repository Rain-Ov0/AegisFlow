#include "aegisflow/net/event_loop.hpp"

#include "aegisflow/base/array_view.hpp"
#include "aegisflow/runtime/cancellation.hpp"
#include "tests/support/test_harness.hpp"

#include <sys/socket.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using aegisflow::test::require;

class NoopTimerScheduler final : public aegisflow::timer::ITimerScheduler {
public:
    [[nodiscard]] aegisflow::timer::TimerScheduleResult scheduleAt(
        aegisflow::timer::SteadyTime,
        std::weak_ptr<aegisflow::timer::ITimerSink>,
        aegisflow::timer::TimerEvent
    ) noexcept override {
        aegisflow::timer::TimerScheduleResult result;
        result.status = aegisflow::timer::TimerStatus::NotRunning;
        return result;
    }

    [[nodiscard]] aegisflow::timer::TimerStatus cancel(
        aegisflow::timer::TimerId
    ) noexcept override {
        return aegisflow::timer::TimerStatus::NotFound;
    }
};

class RecordingBusinessHandler final
    : public aegisflow::net::IFrameBusinessHandler {
public:
    [[nodiscard]] aegisflow::net::FrameBusinessResult handle(
        const aegisflow::base::ArrayView<const std::uint8_t> request_payload,
        const aegisflow::runtime::CancellationToken stop_token
    ) override {
        {
            std::lock_guard lock(mutex_);
            ++calls_;
            worker_thread_ = std::this_thread::get_id();
            request_.assign(request_payload.begin(), request_payload.end());
        }

        if (stop_token.stopRequested()) {
            aegisflow::net::FrameBusinessResult result;
            result.status = aegisflow::net::FrameBusinessStatus::Cancelled;
            return result;
        }

        std::vector<std::uint8_t> response{'r', 'e', 'p', 'l', 'y', ':'};
        response.insert(
            response.end(), request_payload.begin(), request_payload.end());
        aegisflow::net::FrameBusinessResult result;
        result.status = aegisflow::net::FrameBusinessStatus::Response;
        result.response_payload = std::move(response);
        return result;
    }

    [[nodiscard]] std::size_t calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }

    [[nodiscard]] std::thread::id workerThread() const {
        std::lock_guard lock(mutex_);
        return worker_thread_;
    }

    [[nodiscard]] std::vector<std::uint8_t> request() const {
        std::lock_guard lock(mutex_);
        return request_;
    }

private:
    mutable std::mutex mutex_;
    std::size_t calls_ = 0;
    std::thread::id worker_thread_;
    std::vector<std::uint8_t> request_;
};

struct SocketPair {
    aegisflow::net::OwnedSocket loop_side;
    aegisflow::net::OwnedSocket peer_side;
};

[[nodiscard]] aegisflow::runtime::BoundedWorkerPoolConfig poolConfig(
    const std::size_t queue_capacity
) {
    aegisflow::runtime::BoundedWorkerPoolConfig config;
    config.thread_count = 1;
    config.queue_capacity = queue_capacity;
    return config;
}

[[nodiscard]] SocketPair makeSocketPair() {
    std::array<int, 2> descriptors{-1, -1};
    require(
        ::socketpair(
            AF_UNIX,
            SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
            0,
            descriptors.data()
        ) == 0,
        "socketpair 必须创建成功"
    );
    SocketPair sockets;
    sockets.loop_side = aegisflow::net::OwnedSocket(descriptors[0]);
    sockets.peer_side = aegisflow::net::OwnedSocket(descriptors[1]);
    return sockets;
}

[[nodiscard]] std::vector<std::uint8_t> frame(
    const std::string_view payload
) {
    const auto header = aegisflow::net::protocol::encodePayloadLength(
        static_cast<std::uint32_t>(payload.size())
    );
    std::vector<std::uint8_t> bytes(header.begin(), header.end());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

void sendAll(
    const int fd,
    const aegisflow::base::ArrayView<const std::uint8_t> bytes
) {
    std::size_t offset = 0;
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (offset < bytes.size()) {
        const auto sent = ::send(
            fd,
            bytes.data() + offset,
            bytes.size() - offset,
            MSG_NOSIGNAL
        );
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
            std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
            continue;
        }
        require(false, "socketpair 必须写入完整测试帧");
    }
}

[[nodiscard]] bool readAvailable(
    const int fd,
    std::vector<std::uint8_t>& received
) {
    std::array<std::uint8_t, 256> scratch{};
    while (true) {
        const auto bytes = ::recv(fd, scratch.data(), scratch.size(), 0);
        if (bytes > 0) {
            received.insert(
                received.end(),
                scratch.begin(),
                scratch.begin() + bytes
            );
            continue;
        }
        if (bytes == 0) {
            return true;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return false;
        }
        require(false, "socketpair 读取不得失败");
    }
}

[[nodiscard]] aegisflow::net::EventLoopConfig loopConfig(
    const std::uint32_t loop_id
) {
    aegisflow::net::EventLoopConfig config;
    config.loop_id = loop_id;
    config.max_connections = 4;
    config.max_events = 8;
    config.read_scratch_bytes = 16;
    config.completion_capacity = 4;
    config.completion_byte_capacity = 1024;
    config.max_completion_bytes = 256;
    config.deadlines.enabled = false;
    config.session.max_frame_payload_bytes = 128;
    config.session.input_soft_watermark_bytes = 132;
    config.session.max_input_buffer_bytes = 132;
    config.session.output_soft_watermark_bytes = 256;
    config.session.max_output_buffer_bytes = 256;
    return config;
}

void failedSessionConstructionDoesNotRegister() {
    constexpr std::uint32_t kLoopId = 13;
    aegisflow::runtime::BoundedWorkerPoolConfig pool_config;
    pool_config.thread_count = 1;
    pool_config.queue_capacity = 1;
    aegisflow::runtime::BoundedWorkerPool worker_pool(pool_config);
    const auto handler = std::make_shared<RecordingBusinessHandler>();
    const auto router = std::make_shared<aegisflow::net::CompletionRouter>(
        kLoopId, 1
    );
    NoopTimerScheduler timer_scheduler;
    std::atomic<std::size_t> released{0};
    auto config = loopConfig(kLoopId);
    config.session.max_frame_payload_bytes = 0;
    aegisflow::net::EventLoop event_loop(
        config,
        worker_pool,
        handler,
        timer_scheduler,
        nullptr,
        [&released] { released.fetch_add(1, std::memory_order_relaxed); },
        router
    );

    auto sockets = makeSocketPair();
    require(
        event_loop.adopt(std::move(sockets.loop_side)) ==
            aegisflow::net::EventLoopStatus::SystemCallFailed,
        "Session 构造失败必须由 adopt 明确报告"
    );
    std::vector<std::uint8_t> received;
    require(
        readAvailable(sockets.peer_side.get(), received) && received.empty(),
        "Session 构造失败必须关闭已接管的连接"
    );
    require(
        event_loop.shutdown() == aegisflow::net::EventLoopStatus::Ok,
        "Session 构造失败后 EventLoop 仍必须可停止"
    );
    require(
        released.load(std::memory_order_relaxed) == 0,
        "构造失败的 Session 不得进入关停析构路径"
    );
    worker_pool.close();
    require(
        worker_pool.drainUntil(std::chrono::steady_clock::now() + 1s) ==
            aegisflow::runtime::WorkerPoolStatus::Ok,
        "Session 构造失败测试 WorkerPool 必须排空"
    );
    require(
        worker_pool.join() == aegisflow::runtime::WorkerPoolStatus::Ok,
        "Session 构造失败测试 WorkerPool 必须 join"
    );
}

void lengthFrameRunsOnWorkerAndReturnsThroughMailbox() {
    constexpr std::uint32_t kLoopId = 17;
    const auto owner_thread = std::this_thread::get_id();
    aegisflow::runtime::BoundedWorkerPool worker_pool(poolConfig(4));
    const auto handler = std::make_shared<RecordingBusinessHandler>();
    const auto router = std::make_shared<aegisflow::net::CompletionRouter>(
        kLoopId, 1
    );
    const auto drain_control =
        std::make_shared<aegisflow::net::EventLoopDrainControl>();
    NoopTimerScheduler timer_scheduler;
    std::atomic<std::size_t> released{0};
    aegisflow::net::EventLoop event_loop(
        loopConfig(kLoopId),
        worker_pool,
        handler,
        timer_scheduler,
        nullptr,
        [&released] { released.fetch_add(1, std::memory_order_relaxed); },
        router,
        drain_control
    );

    auto sockets = makeSocketPair();
    require(
        event_loop.adopt(std::move(sockets.loop_side)) ==
            aegisflow::net::EventLoopStatus::Ok,
        "EventLoop 必须接管 socketpair 服务端"
    );

    const auto request_frame = frame("request");
    sendAll(
        sockets.peer_side.get(),
        aegisflow::base::ArrayView<const std::uint8_t>(request_frame).first(2)
    );
    require(
        event_loop.pollOnce(100) == aegisflow::net::EventLoopStatus::Ok,
        "EventLoop 必须接收不完整长度头"
    );
    require(handler->calls() == 0, "半帧不得投递业务任务");

    sendAll(
        sockets.peer_side.get(),
        aegisflow::base::ArrayView<const std::uint8_t>(request_frame).subview(2)
    );
    const auto expected_response = frame("reply:request");
    std::vector<std::uint8_t> received;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (received.size() < expected_response.size() &&
           std::chrono::steady_clock::now() < deadline) {
        require(
            event_loop.pollOnce(20) == aegisflow::net::EventLoopStatus::Ok,
            "EventLoop 请求/完成事件轮询必须成功"
        );
        (void)readAvailable(sockets.peer_side.get(), received);
    }

    require(received == expected_response, "客户端必须收到完整长度帧响应");
    require(handler->calls() == 1, "完整请求只能执行一次业务");
    require(handler->request() == std::vector<std::uint8_t>(
                {'r', 'e', 'q', 'u', 'e', 's', 't'}),
            "Worker 必须获得不含长度头的完整 payload");
    require(
        handler->workerThread() != owner_thread,
        "业务处理不得在 EventLoop owner 线程执行"
    );

    require(
        event_loop.shutdown() == aegisflow::net::EventLoopStatus::Ok,
        "EventLoop 必须在 owner 线程停止"
    );
    require(
        released.load(std::memory_order_relaxed) == 1,
        "EventLoop 停止必须归还已接管连接"
    );
    worker_pool.close();
    require(
        worker_pool.drainUntil(std::chrono::steady_clock::now() + 1s) ==
            aegisflow::runtime::WorkerPoolStatus::Ok,
        "WorkerPool 必须排空"
    );
    require(
        worker_pool.join() == aegisflow::runtime::WorkerPoolStatus::Ok,
        "WorkerPool 必须 join"
    );
}

void drainRejectsNewDispatchAndClosesConnection() {
    constexpr std::uint32_t kLoopId = 23;
    aegisflow::runtime::BoundedWorkerPool worker_pool(poolConfig(4));
    const auto handler = std::make_shared<RecordingBusinessHandler>();
    const auto router = std::make_shared<aegisflow::net::CompletionRouter>(
        kLoopId, 1
    );
    const auto drain_control =
        std::make_shared<aegisflow::net::EventLoopDrainControl>();
    NoopTimerScheduler timer_scheduler;
    std::atomic<std::size_t> released{0};
    aegisflow::net::EventLoop event_loop(
        loopConfig(kLoopId),
        worker_pool,
        handler,
        timer_scheduler,
        nullptr,
        [&released] { released.fetch_add(1, std::memory_order_relaxed); },
        router,
        drain_control
    );

    auto sockets = makeSocketPair();
    require(
        event_loop.adopt(std::move(sockets.loop_side)) ==
            aegisflow::net::EventLoopStatus::Ok,
        "drain 测试连接必须接管成功"
    );
    drain_control->requestDrain();
    const auto request_frame = frame("must-not-run");
    sendAll(sockets.peer_side.get(), request_frame);

    const auto deadline = std::chrono::steady_clock::now() + 1s;
    std::vector<std::uint8_t> unexpected_output;
    bool peer_closed = false;
    while (!peer_closed && std::chrono::steady_clock::now() < deadline) {
        require(
            event_loop.pollOnce(20) == aegisflow::net::EventLoopStatus::Ok,
            "drain 期间 EventLoop 轮询必须成功"
        );
        peer_closed = readAvailable(
            sockets.peer_side.get(), unexpected_output
        );
    }

    require(peer_closed, "drain 必须关闭新请求所在连接");
    require(unexpected_output.empty(), "drain 后不得为新请求生成响应");
    require(handler->calls() == 0, "drain 后新请求不得投递到 Worker");
    require(worker_pool.inflightCount() == 0, "drain 后不得留下业务 inflight");
    require(
        released.load(std::memory_order_relaxed) == 1,
        "drain 关闭连接必须归还一次连接计数"
    );

    require(
        event_loop.shutdown() == aegisflow::net::EventLoopStatus::Ok,
        "drain 后 EventLoop 必须可停止"
    );
    worker_pool.close();
    require(
        worker_pool.drainUntil(std::chrono::steady_clock::now() + 1s) ==
            aegisflow::runtime::WorkerPoolStatus::Ok,
        "drain 测试 WorkerPool 必须排空"
    );
    require(
        worker_pool.join() == aegisflow::runtime::WorkerPoolStatus::Ok,
        "drain 测试 WorkerPool 必须 join"
    );
}

void ownerThreadBoundaryRejectsForeignAdoptAndPoll() {
    constexpr std::uint32_t kLoopId = 29;
    aegisflow::runtime::BoundedWorkerPool worker_pool(poolConfig(1));
    const auto handler = std::make_shared<RecordingBusinessHandler>();
    const auto router = std::make_shared<aegisflow::net::CompletionRouter>(
        kLoopId, 1
    );
    NoopTimerScheduler timer_scheduler;
    aegisflow::net::EventLoop event_loop(
        loopConfig(kLoopId),
        worker_pool,
        handler,
        timer_scheduler,
        nullptr,
        {},
        router
    );
    auto sockets = makeSocketPair();

    auto adopt_status = aegisflow::net::EventLoopStatus::Ok;
    auto poll_status = aegisflow::net::EventLoopStatus::Ok;
    std::thread foreign_thread(
        [&event_loop,
         socket = std::move(sockets.loop_side),
         &adopt_status,
         &poll_status]() mutable {
            adopt_status = event_loop.adopt(std::move(socket));
            poll_status = event_loop.pollOnce(0);
        }
    );
    foreign_thread.join();

    require(
        adopt_status == aegisflow::net::EventLoopStatus::WrongThread &&
            poll_status == aegisflow::net::EventLoopStatus::WrongThread,
        "adopt/poll 只能由 EventLoop owner 线程调用"
    );
    std::vector<std::uint8_t> received;
    require(
        readAvailable(sockets.peer_side.get(), received) && received.empty(),
        "错线程 adopt 失败后必须释放转移的 socket"
    );
    require(
        event_loop.shutdown() == aegisflow::net::EventLoopStatus::Ok,
        "owner 线程仍必须能停止 EventLoop"
    );
    worker_pool.close();
    require(
        worker_pool.drainUntil(std::chrono::steady_clock::now() + 1s) ==
            aegisflow::runtime::WorkerPoolStatus::Ok,
        "owner 边界测试 WorkerPool 必须排空"
    );
    require(
        worker_pool.join() == aegisflow::runtime::WorkerPoolStatus::Ok,
        "owner 边界测试 WorkerPool 必须 join"
    );
}

}  // namespace

int main() {
    return aegisflow::test::runModule(
        "event_loop",
        {
            {"Session 构造失败回收", failedSessionConstructionDoesNotRegister},
            {"字节帧 Worker 往返", lengthFrameRunsOnWorkerAndReturnsThroughMailbox},
            {"drain 拒绝新业务", drainRejectsNewDispatchAndClosesConnection},
            {"owner 线程边界", ownerThreadBoundaryRejectsForeignAdoptAndPoll},
        }
    );
}
