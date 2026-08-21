#include "aegisflow/net/completion_router.hpp"

#include "aegisflow/net/loop_mailbox.hpp"
#include "aegisflow/net/protocol_contract.hpp"

#include <limits>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace aegisflow::net {
namespace {

[[nodiscard]] BusinessCompletionStatus completionStatusFor(
    const FrameBusinessStatus status
) noexcept {
    switch (status) {
        case FrameBusinessStatus::Response:
            return BusinessCompletionStatus::Response;
        case FrameBusinessStatus::InvalidRequest:
            return BusinessCompletionStatus::InvalidRequest;
        case FrameBusinessStatus::Cancelled:
            return BusinessCompletionStatus::Cancelled;
        case FrameBusinessStatus::Failed:
            return BusinessCompletionStatus::Failed;
    }
    return BusinessCompletionStatus::Failed;
}

class BusinessTask final : public runtime::IWorkerTask {
public:
    BusinessTask(
        BusinessRequest request,
        std::shared_ptr<IFrameBusinessHandler> handler,
        std::weak_ptr<CompletionRouter> router,
        const std::size_t max_payload
    ) : request_(std::move(request)),
        handler_(std::move(handler)),
        router_(std::move(router)),
        max_payload_(max_payload) {}

    void run(const runtime::CancellationToken stop_token) const override {
        BusinessCompletion completion;
        completion.token = request_.token;
        try {
            auto result = handler_->handle(
                request_.request_payload,
                stop_token
            );
            completion.status = completionStatusFor(result.status);
            if (result.status == FrameBusinessStatus::Response) {
                if (result.response_payload.empty() ||
                    result.response_payload.size() > max_payload_) {
                    completion.status = result.response_payload.empty()
                        ? BusinessCompletionStatus::Failed
                        : BusinessCompletionStatus::OutputLimitExceeded;
                } else {
                    const auto header = protocol::encodePayloadLength(
                        static_cast<std::uint32_t>(result.response_payload.size())
                    );
                    completion.response_frame.reserve(
                        header.size() + result.response_payload.size()
                    );
                    completion.response_frame.insert(
                        completion.response_frame.end(),
                        header.begin(),
                        header.end()
                    );
                    completion.response_frame.insert(
                        completion.response_frame.end(),
                        result.response_payload.begin(),
                        result.response_payload.end()
                    );
                }
            }
        } catch (...) {
            completion.status = BusinessCompletionStatus::Failed;
            completion.response_frame.clear();
        }
        if (auto router = router_.lock(); router != nullptr) {
            // Worker 不直接触碰 Session，只将自有 completion 投递回 token 所属 loop。
            router->tryRoute(std::move(completion));
        }
    }

private:
    BusinessRequest request_;
    std::shared_ptr<IFrameBusinessHandler> handler_;
    std::weak_ptr<CompletionRouter> router_;
    std::size_t max_payload_ = 0;
};

}  // 命名空间

std::unique_ptr<runtime::IWorkerTask> makeBusinessTask(
    BusinessRequest request,
    std::shared_ptr<IFrameBusinessHandler> handler,
    std::weak_ptr<CompletionRouter> completion_router,
    const std::size_t max_response_payload_bytes
) {
    return std::make_unique<BusinessTask>(
        std::move(request),
        std::move(handler),
        std::move(completion_router),
        max_response_payload_bytes
    );
}

class CompletionRouter::Impl final {
public:
    struct LoopSlot {
        explicit LoopSlot(const std::uint32_t id) noexcept
            : loop_id(id) {}

        std::uint32_t loop_id = 0;
        std::weak_ptr<LoopMailbox> mailbox;
        bool bound = false;
    };

    Impl(
        const std::uint32_t first_id,
        const std::size_t count
    ) : first_loop_id(first_id) {
        const auto remaining_ids =
            static_cast<std::uint64_t>(
                std::numeric_limits<std::uint32_t>::max()
            ) - static_cast<std::uint64_t>(first_id) + 1U;
        if (count == 0 ||
            static_cast<std::uint64_t>(count) > remaining_ids) {
            throw std::invalid_argument("CompletionRouter loop 范围无效");
        }

        loops.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            loops.push_back(std::make_unique<LoopSlot>(
                static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(first_id) + index
                )
            ));
        }
    }

    [[nodiscard]] std::size_t indexFor(
        const std::uint32_t loop_id
    ) const noexcept {
        if (loop_id < first_loop_id) {
            return loops.size();
        }
        const auto offset = static_cast<std::uint64_t>(loop_id) -
                            static_cast<std::uint64_t>(first_loop_id);
        if (offset >= loops.size()) {
            return loops.size();
        }
        return static_cast<std::size_t>(offset);
    }

    mutable std::shared_mutex mutex;
    std::vector<std::unique_ptr<LoopSlot>> loops;
    std::uint32_t first_loop_id = 0;
    bool accepting = true;
};

CompletionRouter::CompletionRouter(
    const std::uint32_t first_loop_id,
    const std::size_t loop_count
) : impl_(std::make_unique<Impl>(first_loop_id, loop_count)) {}

CompletionRouter::~CompletionRouter() = default;

bool CompletionRouter::bind(
    const std::uint32_t loop_id,
    std::shared_ptr<LoopMailbox> mailbox
) noexcept {
    std::unique_lock lock(impl_->mutex);
    if (mailbox == nullptr) {
        return false;
    }
    const auto index = impl_->indexFor(loop_id);
    if (index == impl_->loops.size()) {
        return false;
    }
    if (!impl_->accepting) {
        return false;
    }

    auto& slot = *impl_->loops[index];
    if (slot.bound) {
        if (const auto current = slot.mailbox.lock(); current != nullptr) {
            return current == mailbox;
        }
    }
    slot.mailbox = std::move(mailbox);
    slot.bound = true;
    return true;
}

void CompletionRouter::unbind(
    const std::uint32_t loop_id,
    const std::shared_ptr<LoopMailbox>& expected_mailbox
) noexcept {
    std::unique_lock lock(impl_->mutex);
    if (expected_mailbox == nullptr) {
        return;
    }
    const auto index = impl_->indexFor(loop_id);
    if (index == impl_->loops.size()) {
        return;
    }

    auto& slot = *impl_->loops[index];
    if (!slot.bound) {
        return;
    }
    if (const auto current = slot.mailbox.lock();
        current != nullptr && current != expected_mailbox) {
        return;
    }
    slot.mailbox.reset();
    slot.bound = false;
}

void CompletionRouter::tryRoute(
    BusinessCompletion completion
) noexcept {
    if (!completion.valid()) {
        return;
    }

    // weak mailbox 使 router 不延长 EventLoop 寿命；关停后的迟到结果直接丢弃。
    std::shared_lock lock(impl_->mutex);
    if (!impl_->accepting) {
        return;
    }
    const auto index = impl_->indexFor(completion.token.loop_id);
    if (index == impl_->loops.size()) {
        return;
    }

    auto& slot = *impl_->loops[index];
    const auto mailbox = slot.bound ? slot.mailbox.lock() : nullptr;
    if (mailbox == nullptr) {
        return;
    }

    (void)mailbox->tryPost(std::move(completion));
}

void CompletionRouter::close() noexcept {
    std::unique_lock lock(impl_->mutex);
    impl_->accepting = false;
}

}  // 命名空间 aegisflow::net
