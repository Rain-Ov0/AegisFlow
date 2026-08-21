#pragma once

#include "aegisflow/net/event_loop_group.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace aegisflow::net {

enum class AcceptorLoopStatus {
    Ok,
    InvalidState,
    AddressInUse,
    SystemCallFailed,
    StartFailed,
    SelfJoin,
};

struct AcceptorLoopConfig {
    std::string bind_address = "0.0.0.0";
    std::uint16_t port = 8080;
    int backlog = 512;
    std::size_t max_connections = 65536;
    std::size_t max_events = 16;

    bool operator==(const AcceptorLoopConfig& other) const {
        return bind_address == other.bind_address &&
               port == other.port &&
               backlog == other.backlog &&
               max_connections == other.max_connections &&
               max_events == other.max_events;
    }
};

class AcceptorLoop final {
public:
    AcceptorLoop(
        AcceptorLoopConfig config,
        EventLoopGroup& event_loops
    );
    ~AcceptorLoop();

    AcceptorLoop(const AcceptorLoop&) = delete;
    AcceptorLoop& operator=(const AcceptorLoop&) = delete;
    AcceptorLoop(AcceptorLoop&&) = delete;
    AcceptorLoop& operator=(AcceptorLoop&&) = delete;

    [[nodiscard]] AcceptorLoopStatus start() noexcept;
    void stop() noexcept;
    [[nodiscard]] AcceptorLoopStatus join() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // 命名空间 aegisflow::net
