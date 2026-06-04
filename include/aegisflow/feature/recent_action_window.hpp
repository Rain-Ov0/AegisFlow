#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "event.pb.h"

namespace aegisflow::feature {

struct RecentAction {
    aegisflow::v1::EventType type = aegisflow::v1::EVENT_TYPE_UNKNOWN;
    aegisflow::v1::EventResult result = aegisflow::v1::EVENT_RESULT_UNKNOWN;
    uint64_t timestamp_ms = 0;
};

template<size_t N>
class RecentActionWindow {
public:
    static_assert(N > 0, "RecentActionWindow capacity must be greater than 0");

    void push(const RecentAction& action) {
        actions_[head_] = action;
        head_ = (head_ + 1) % N;
        if (size_ < N) {
            ++ size_;
        }
    }

    [[nodiscard]] std::vector<RecentAction> list() const {
        std::vector<RecentAction> result;
        result.reserve(size_);
        const size_t start = size_ == N ? head_ : 0;
        for (size_t i = 0; i < size_; ++ i ) {
            result.push_back(actions_[(start + i) % N]);
        }
        return result;
    }

    [[nodiscard]] size_t size() const {
        return size_;
    }

    [[nodiscard]] size_t capacity() const {
        return N;
    }

    [[nodiscard]] bool empty() const {
        return size_ == 0;
    }

private:
    std::array<RecentAction, N> actions_;
    size_t head_ = 0;
    size_t size_ = 0;
};

}