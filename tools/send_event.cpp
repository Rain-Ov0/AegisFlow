#include "aegisflow/net/protocol_contract.hpp"

#include "login.pb.h"

#include <google/protobuf/stubs/common.h>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace protocol = aegisflow::net::protocol;
namespace login = aegisflow::login;

namespace {

constexpr auto kIoTimeout = timeval{5, 0};

class Socket final {
public:
    explicit Socket(const int descriptor = -1) noexcept
        : descriptor_(descriptor) {}

    ~Socket() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {}

    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            if (descriptor_ >= 0) {
                ::close(descriptor_);
            }
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

private:
    int descriptor_ = -1;
};

struct AddrInfoDeleter {
    void operator()(addrinfo* addresses) const noexcept {
        if (addresses != nullptr) {
            ::freeaddrinfo(addresses);
        }
    }
};

[[noreturn]] void throwSystemError(const std::string_view operation) {
    throw std::runtime_error(
        std::string(operation) + ": " + std::strerror(errno)
    );
}

Socket connectSocket(
    const std::string& host,
    const std::string& port
) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* raw_addresses = nullptr;
    const int resolve_status = ::getaddrinfo(
        host.c_str(),
        port.c_str(),
        &hints,
        &raw_addresses
    );
    if (resolve_status != 0) {
        throw std::runtime_error(
            "地址解析失败: " + std::string(::gai_strerror(resolve_status))
        );
    }
    std::unique_ptr<addrinfo, AddrInfoDeleter> addresses(raw_addresses);

    int last_error = ECONNREFUSED;
    for (auto* address = addresses.get();
         address != nullptr;
         address = address->ai_next) {
        const int descriptor = ::socket(
            address->ai_family,
            address->ai_socktype | SOCK_CLOEXEC,
            address->ai_protocol
        );
        if (descriptor < 0) {
            last_error = errno;
            continue;
        }

        Socket socket(descriptor);
        int connected = 0;
        do {
            connected = ::connect(
                socket.get(), address->ai_addr, address->ai_addrlen);
        } while (connected != 0 && errno == EINTR);
        if (connected == 0 &&
            ::setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO,
                         &kIoTimeout, sizeof(kIoTimeout)) == 0 &&
            ::setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO,
                         &kIoTimeout, sizeof(kIoTimeout)) == 0) {
            return socket;
        }
        last_error = errno;
    }

    errno = last_error;
    throwSystemError("连接服务端失败");
}

void sendAll(
    const int descriptor,
    const void* data,
    const std::size_t size
) {
    const auto* bytes = static_cast<const char*>(data);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t sent = ::send(
            descriptor,
            bytes + offset,
            size - offset,
            MSG_NOSIGNAL
        );
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent == 0) {
            throw std::runtime_error("发送期间连接关闭");
        }
        throwSystemError("send");
    }
}

void receiveExact(
    const int descriptor,
    void* data,
    const std::size_t size
) {
    auto* bytes = static_cast<char*>(data);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t received = ::recv(
            descriptor,
            bytes + offset,
            size - offset,
            0
        );
        if (received > 0) {
            offset += static_cast<std::size_t>(received);
            continue;
        }
        if (received == 0) {
            throw std::runtime_error("接收期间连接关闭");
        }
        if (errno == EINTR) {
            continue;
        }
        throwSystemError("recv");
    }
}

std::uint64_t nowMilliseconds() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

const char* actionName(const login::DecisionAction action) noexcept {
    switch (action) {
    case login::PASS:
        return "PASS";
    case login::REVIEW:
        return "REVIEW";
    case login::REJECT:
        return "REJECT";
    default:
        return "UNKNOWN";
    }
}

const char* statusName(const login::ResponseStatus status) noexcept {
    switch (status) {
    case login::RESPONSE_STATUS_OK:
        return "OK";
    case login::RESPONSE_STATUS_OVERLOADED:
        return "OVERLOADED";
    case login::RESPONSE_STATUS_TIMEOUT:
        return "TIMEOUT";
    default:
        return "UNKNOWN";
    }
}

login::LoginResponse exchange(
    const std::string& host,
    const std::string& port,
    const login::LoginRequest& request
) {
    std::string payload;
    if (!request.SerializeToString(&payload)) {
        throw std::runtime_error("序列化 LoginRequest 失败");
    }
    if (protocol::validatePayloadLength(
            static_cast<std::uint32_t>(payload.size())
        ) != protocol::ProtocolError::none) {
        throw std::runtime_error("请求载荷长度超出协议范围");
    }

    auto socket = connectSocket(host, port);
    const auto header = protocol::encodePayloadLength(
        static_cast<std::uint32_t>(payload.size())
    );
    sendAll(socket.get(), header.data(), header.size());
    sendAll(socket.get(), payload.data(), payload.size());

    protocol::FrameHeader response_header{};
    receiveExact(
        socket.get(),
        response_header.data(),
        response_header.size()
    );
    const std::uint32_t response_size =
        protocol::decodePayloadLength(response_header);
    if (protocol::validatePayloadLength(response_size) !=
        protocol::ProtocolError::none) {
        throw std::runtime_error("服务端响应帧长度非法");
    }

    std::string response_payload(response_size, '\0');
    receiveExact(
        socket.get(),
        response_payload.data(),
        response_payload.size()
    );

    login::LoginResponse response;
    if (!response.ParseFromString(response_payload)) {
        throw std::runtime_error("解析 LoginResponse 失败");
    }
    return response;
}

int runClient(const int argc, char* argv[]) {
    const std::string host = argc >= 2 ? argv[1] : "127.0.0.1";
    const std::string port = argc >= 3 ? argv[2] : "8080";
    std::string user_id = "user_001";
    if (argc >= 4) {
        user_id = std::string_view(argv[3]) == "--empty-user"
                      ? ""
                      : argv[3];
    }

    login::LoginRequest request;
    request.set_attempt_id(1001);
    request.set_timestamp_ms(nowMilliseconds());
    request.set_user_id(user_id);
    request.set_ip("127.0.0.1");
    request.set_device_id("device_demo_001");
    request.set_result(login::SUCCESS);

    const auto response = exchange(host, port, request);
    std::cout << "status=" << statusName(response.status()) << '\n'
              << "reason_code=" << response.reason_code() << '\n';

    if (response.has_decision()) {
        const auto& decision = response.decision();
        std::cout << "attempt_id=" << decision.attempt_id() << '\n'
                  << "user_id=" << decision.user_id() << '\n'
                  << "action=" << actionName(decision.action()) << '\n'
                  << "risk_score=" << decision.risk_score() << '\n'
                  << "cost_us=" << decision.cost_us() << '\n';

        for (const auto& hit : decision.policy_hits()) {
            std::cout << "policy reason=" << hit.reason_code()
                      << " action=" << actionName(hit.action())
                      << " risk_score=" << hit.risk_score() << '\n';
        }
    } else if (response.status() == login::RESPONSE_STATUS_OK) {
        throw std::runtime_error("成功响应缺少登录决策");
    }

    return response.status() == login::RESPONSE_STATUS_OK ? 0 : 2;
}

}  // 命名空间

int main(const int argc, char* argv[]) {
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    try {
        const int exit_code = runClient(argc, argv);
        google::protobuf::ShutdownProtobufLibrary();
        return exit_code;
    } catch (const std::exception& error) {
        std::cerr << "send_event 失败: " << error.what() << std::endl;
        google::protobuf::ShutdownProtobufLibrary();
        return 1;
    }
}
