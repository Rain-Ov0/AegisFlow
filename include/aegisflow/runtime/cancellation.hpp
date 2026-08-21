#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace aegisflow::runtime {

namespace detail {

struct CancellationState {
    std::atomic<bool> stop_requested{false};
};

}  // namespace detail

class CancellationSource;

class CancellationToken {
public:
    CancellationToken() noexcept = default;

    [[nodiscard]] bool stopRequested() const noexcept {
        return state_ != nullptr &&
               state_->stop_requested.load(std::memory_order_acquire);
    }

private:
    friend class CancellationSource;

    explicit CancellationToken(
        std::shared_ptr<detail::CancellationState> state
    ) noexcept : state_(std::move(state)) {}

    std::shared_ptr<detail::CancellationState> state_;
};

class CancellationSource {
public:
    CancellationSource()
        : state_(std::make_shared<detail::CancellationState>()) {}

    [[nodiscard]] CancellationToken token() const noexcept {
        return CancellationToken(state_);
    }

    [[nodiscard]] bool requestStop() noexcept {
        return state_ != nullptr &&
               !state_->stop_requested.exchange(
                   true,
                   std::memory_order_acq_rel
               );
    }

private:
    std::shared_ptr<detail::CancellationState> state_;
};

}  // namespace aegisflow::runtime
