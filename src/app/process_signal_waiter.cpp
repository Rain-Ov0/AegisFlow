#include "aegisflow/app/process_signal_waiter.hpp"

#include <pthread.h>

#include <cerrno>

namespace aegisflow::app {

ProcessSignalWaiter::ProcessSignalWaiter() noexcept {
    (void)::sigemptyset(&signal_set_);
    (void)::sigaddset(&signal_set_, SIGINT);
    (void)::sigaddset(&signal_set_, SIGTERM);
}

bool ProcessSignalWaiter::blockTerminationSignals() noexcept {
    std::lock_guard lock(mutex_);
    if (blocked_) {
        return true;
    }
    const int status = ::pthread_sigmask(
        SIG_BLOCK,
        &signal_set_,
        nullptr
    );
    if (status != 0) {
        return false;
    }
    // 终止信号掩码保持到进程退出，避免宽限停机期间的重复信号打断回收。
    blocked_ = true;
    return true;
}

ProcessSignalResult ProcessSignalWaiter::wait() noexcept {
    {
        std::lock_guard lock(mutex_);
        if (!blocked_ || wait_in_progress_) {
            ProcessSignalResult result;
            result.system_error = EINVAL;
            return result;
        }
        wait_in_progress_ = true;
    }

    int signal_number = 0;
    const int status = ::sigwait(&signal_set_, &signal_number);

    {
        std::lock_guard lock(mutex_);
        wait_in_progress_ = false;
    }
    if (status != 0) {
        ProcessSignalResult result;
        result.system_error = status;
        return result;
    }
    ProcessSignalResult result;
    result.ok = true;
    result.signal_number = signal_number;
    return result;
}

}  // 命名空间 aegisflow::app
