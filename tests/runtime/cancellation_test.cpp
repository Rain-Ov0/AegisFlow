#include "aegisflow/runtime/cancellation.hpp"

#include "tests/support/test_harness.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using aegisflow::runtime::CancellationSource;
using aegisflow::runtime::CancellationToken;
using aegisflow::test::require;

void defaultTokenAndNewSourceAreNotStopped() {
    const CancellationToken unassociated;
    require(!unassociated.stopRequested(), "未关联 token 不得报告停止");

    const CancellationSource source;
    require(!source.token().stopRequested(), "新 source 必须处于未停止状态");
}

void sourceAndTokenCopiesShareOneStopState() {
    CancellationSource source;
    CancellationSource source_copy = source;
    const CancellationToken token = source.token();
    const CancellationToken token_copy = token;

    require(source_copy.requestStop(), "首次停止请求必须返回 true");
    require(token.stopRequested() && token_copy.stopRequested(),
            "source/token 副本必须共享停止状态");
    require(!source.requestStop() && !source_copy.requestStop(),
            "重复停止请求必须返回 false");
}

void tokenKeepsStateAliveAfterSourceDestruction() {
    CancellationToken token;
    {
        CancellationSource source;
        token = source.token();
        require(source.requestStop(), "source 销毁前的停止请求必须成功");
    }
    require(token.stopRequested(), "source 销毁后 token 必须仍可读共享状态");
}

void concurrentReadersObserveTheStopRequest() {
    constexpr std::size_t reader_count = 8;
    CancellationSource source;
    const CancellationToken token = source.token();
    std::atomic<std::size_t> ready{0};
    std::atomic<std::size_t> observed{0};
    std::vector<std::thread> readers;
    readers.reserve(reader_count);

    for (std::size_t index = 0; index < reader_count; ++index) {
        readers.emplace_back([token, &ready, &observed]() {
            ready.fetch_add(1, std::memory_order_release);
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (!token.stopRequested() &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            if (token.stopRequested()) {
                observed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    const auto ready_deadline = std::chrono::steady_clock::now() + 2s;
    while (ready.load(std::memory_order_acquire) != reader_count &&
           std::chrono::steady_clock::now() < ready_deadline) {
        std::this_thread::yield();
    }
    const bool all_ready =
        ready.load(std::memory_order_acquire) == reader_count;
    const bool first_request = source.requestStop();

    for (auto& reader : readers) {
        reader.join();
    }
    require(all_ready, "并发读线程未在期限内启动");
    require(first_request, "并发停止请求必须成功");
    require(observed.load(std::memory_order_relaxed) == reader_count,
            "所有并发读线程必须观察到停止请求");
}

}  // namespace

int main() {
    return aegisflow::test::runModule(
        "cancellation",
        {
            {"初始状态", defaultTokenAndNewSourceAreNotStopped},
            {"副本共享", sourceAndTokenCopiesShareOneStopState},
            {"source 销毁后 token 有效", tokenKeepsStateAliveAfterSourceDestruction},
            {"并发可见性", concurrentReadersObserveTheStopRequest},
        }
    );
}
