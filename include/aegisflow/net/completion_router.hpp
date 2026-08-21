#pragma once

#include "aegisflow/base/array_view.hpp"
#include "aegisflow/net/connection_token.hpp"
#include "aegisflow/runtime/bounded_worker_pool.hpp"
#include "aegisflow/runtime/cancellation.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace aegisflow::net {

enum class FrameBusinessStatus : std::uint8_t {
    Response,
    InvalidRequest,
    Failed,
    Cancelled,
};

struct FrameBusinessResult {
    FrameBusinessStatus status = FrameBusinessStatus::Failed;
    std::vector<std::uint8_t> response_payload;
};

class IFrameBusinessHandler {
public:
    virtual ~IFrameBusinessHandler() = default;

    [[nodiscard]] virtual FrameBusinessResult handle(
        base::ArrayView<const std::uint8_t> request_payload,
        runtime::CancellationToken stop_token
    ) = 0;
};

struct BusinessRequest {
    ConnectionToken token;
    std::vector<std::uint8_t> request_payload;
};

enum class BusinessCompletionStatus : std::uint8_t {
    Response,
    InvalidRequest,
    OutputLimitExceeded,
    Failed,
    Cancelled,
};

struct BusinessCompletion {
    ConnectionToken token;
    BusinessCompletionStatus status = BusinessCompletionStatus::Failed;
    std::vector<std::uint8_t> response_frame;

    [[nodiscard]] bool valid() const noexcept {
        const bool response_shape_valid =
            status == BusinessCompletionStatus::Response
                ? !response_frame.empty()
                : response_frame.empty();
        return token.valid() && response_shape_valid;
    }
};

class LoopMailbox;
class CompletionRouter;

[[nodiscard]] std::unique_ptr<runtime::IWorkerTask> makeBusinessTask(
    BusinessRequest request,
    std::shared_ptr<IFrameBusinessHandler> handler,
    std::weak_ptr<CompletionRouter> completion_router,
    std::size_t max_response_payload_bytes
);

class CompletionRouter final {
public:
    CompletionRouter(std::uint32_t first_loop_id, std::size_t loop_count);
    ~CompletionRouter();

    CompletionRouter(const CompletionRouter&) = delete;
    CompletionRouter& operator=(const CompletionRouter&) = delete;
    CompletionRouter(CompletionRouter&&) = delete;
    CompletionRouter& operator=(CompletionRouter&&) = delete;

    [[nodiscard]] bool bind(
        std::uint32_t loop_id,
        std::shared_ptr<LoopMailbox> mailbox
    ) noexcept;
    void unbind(
        std::uint32_t loop_id,
        const std::shared_ptr<LoopMailbox>& expected_mailbox
    ) noexcept;
    void tryRoute(BusinessCompletion completion) noexcept;
    void close() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // 命名空间 aegisflow::net
