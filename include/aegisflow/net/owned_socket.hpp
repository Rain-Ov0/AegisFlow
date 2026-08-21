#pragma once

#include <unistd.h>

namespace aegisflow::net {

class OwnedSocket final {
public:
    OwnedSocket() noexcept = default;
    explicit OwnedSocket(const int fd) noexcept : fd_(fd) {}
    ~OwnedSocket() { reset(); }

    OwnedSocket(const OwnedSocket&) = delete;
    OwnedSocket& operator=(const OwnedSocket&) = delete;
    OwnedSocket(OwnedSocket&& other) noexcept : fd_(other.release()) {}
    OwnedSocket& operator=(OwnedSocket&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] int release() noexcept {
        const int released = fd_;
        fd_ = -1;
        return released;
    }
    void reset(const int fd = -1) noexcept {
        if (fd_ >= 0) {
            const int closing_fd = fd_;
            fd_ = -1;
            (void)::close(closing_fd);
        }
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

}  // 命名空间 aegisflow::net
