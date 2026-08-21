#pragma once

#include <signal.h>

#include <mutex>

namespace aegisflow::app {

struct ProcessSignalResult {
    bool ok = false;
    int signal_number = 0;
    int system_error = 0;
};

class ProcessSignalWaiter final {
public:
    ProcessSignalWaiter() noexcept;

    ProcessSignalWaiter(const ProcessSignalWaiter&) = delete;
    ProcessSignalWaiter& operator=(const ProcessSignalWaiter&) = delete;

    [[nodiscard]] bool blockTerminationSignals() noexcept;
    [[nodiscard]] ProcessSignalResult wait() noexcept;

private:
    std::mutex mutex_;
    sigset_t signal_set_{};
    bool blocked_ = false;
    bool wait_in_progress_ = false;
};

}  // 命名空间 aegisflow::app
