#include "aegisflow/benchmark/load_generator.hpp"

#include "aegisflow/benchmark/login_frame_codec.hpp"
#include "aegisflow/net/protocol_contract.hpp"

#include "login.pb.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace aegisflow::benchmark {
namespace {

using Clock = std::chrono::steady_clock;
using Byte = std::uint8_t;
using Bytes = std::vector<Byte>;
namespace protocol = aegisflow::net::protocol;
namespace login = aegisflow::login;

struct Endpoint {
    sockaddr_storage address{};
    socklen_t size = 0;
    int family = AF_UNSPEC;
    int socket_type = SOCK_STREAM;
    int protocol = 0;
};

struct Connection {
    int fd = -1;
    std::size_t requests = 0;

    Connection() = default;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&& other) noexcept
        : fd(std::exchange(other.fd, -1)), requests(other.requests) {}
    Connection& operator=(Connection&& other) noexcept {
        if (this != &other) {
            close();
            fd = std::exchange(other.fd, -1);
            requests = other.requests;
        }
        return *this;
    }
    ~Connection() { close(); }

    void close() noexcept {
        if (fd >= 0) {
            (void)::close(fd);
            fd = -1;
        }
        requests = 0;
    }
};

struct WorkerState {
    std::vector<Connection> connections;
};

struct ExchangeResult {
    FailureClass failure = FailureClass::Count;
    login::LoginResponse response;
};

[[nodiscard]] ExchangeResult failedExchange(
    const FailureClass failure
) {
    ExchangeResult result;
    result.failure = failure;
    return result;
}

[[nodiscard]] ExchangeResult successfulExchange(
    login::LoginResponse response
) {
    ExchangeResult result;
    result.response = std::move(response);
    return result;
}

struct PhaseCounters {
    std::uint64_t issued_requests = 0;
    std::uint64_t decoded_responses = 0;
    std::uint64_t failed_requests = 0;
    std::uint64_t connection_attempts = 0;
    std::uint64_t connections_established = 0;
    std::uint64_t planned_reconnects = 0;
    std::array<std::uint64_t, 3> service_status_counts{};
    std::array<std::uint64_t, 3> decision_action_counts{};
    std::array<std::uint64_t, 8> policy_hit_counts{};
    FailureCounters failures;
    std::vector<std::uint64_t> latencies;
    std::vector<std::uint64_t> schedule_lags;
};

struct PhaseResult {
    PhaseCounters counters;
    std::uint64_t elapsed_us = 0;
    std::uint64_t max_outstanding_requests = 0;
};

enum class WaitResult {
    Ready,
    Timeout,
    Closed,
    Error,
};

[[nodiscard]] std::uint64_t epochMillis() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

[[nodiscard]] std::uint64_t elapsedMicros(
    const Clock::time_point begin,
    const Clock::time_point end
) noexcept {
    return end <= begin
               ? 0
               : static_cast<std::uint64_t>(
                     std::chrono::duration_cast<std::chrono::microseconds>(
                         end - begin
                     ).count()
                 );
}

[[nodiscard]] std::uint64_t splitMix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] bool choose(
    const std::uint64_t seed,
    const std::uint64_t ordinal,
    const double ratio
) noexcept {
    if (ratio <= 0.0) {
        return false;
    }
    if (ratio >= 1.0) {
        return true;
    }
    const auto unit = static_cast<long double>(splitMix64(seed ^ ordinal)) /
                      std::numeric_limits<std::uint64_t>::max();
    return unit < ratio;
}

[[nodiscard]] bool containsWhitespace(const std::string& value) noexcept {
    return std::any_of(value.begin(), value.end(), [](const unsigned char byte) {
        return std::isspace(byte) != 0;
    });
}

[[nodiscard]] bool isIpAddress(const std::string& value) noexcept {
    in_addr ipv4{};
    in6_addr ipv6{};
    return ::inet_pton(AF_INET, value.c_str(), &ipv4) == 1 ||
           ::inet_pton(AF_INET6, value.c_str(), &ipv6) == 1;
}

[[nodiscard]] Endpoint resolveEndpoint(const LoadConfig& config) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICSERV;
    addrinfo* addresses = nullptr;
    const auto port = std::to_string(config.port);
    const int status = ::getaddrinfo(
        config.host.c_str(), port.c_str(), &hints, &addresses
    );
    if (status != 0) {
        throw std::runtime_error(
            "无法解析 benchmark 目标: " +
            std::string(::gai_strerror(status))
        );
    }

    Endpoint endpoint;
    for (auto* current = addresses; current != nullptr;
         current = current->ai_next) {
        if (current->ai_addrlen > sizeof(endpoint.address)) {
            continue;
        }
        std::memcpy(&endpoint.address, current->ai_addr, current->ai_addrlen);
        endpoint.size = static_cast<socklen_t>(current->ai_addrlen);
        endpoint.family = current->ai_family;
        endpoint.socket_type = current->ai_socktype;
        endpoint.protocol = current->ai_protocol;
        break;
    }
    ::freeaddrinfo(addresses);
    if (endpoint.size == 0) {
        throw std::runtime_error("benchmark 目标没有可用地址");
    }
    return endpoint;
}

[[nodiscard]] int remainingMillis(
    const Clock::time_point deadline
) noexcept {
    const auto remaining = deadline - Clock::now();
    if (remaining <= Clock::duration::zero()) {
        return 0;
    }
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        remaining
    ).count();
    if (milliseconds == 0) {
        milliseconds = 1;
    }
    return static_cast<int>(std::clamp<std::int64_t>(
        milliseconds, 1, INT_MAX
    ));
}

[[nodiscard]] WaitResult waitFd(
    const int fd,
    const short events,
    const Clock::time_point deadline
) noexcept {
    while (Clock::now() < deadline) {
        pollfd item{fd, events, 0};
        const int ready = ::poll(&item, 1, remainingMillis(deadline));
        if (ready > 0) {
            if ((item.revents & events) != 0) {
                return WaitResult::Ready;
            }
            if ((item.revents & POLLHUP) != 0) {
                return WaitResult::Closed;
            }
            return WaitResult::Error;
        }
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        return ready == 0 ? WaitResult::Timeout : WaitResult::Error;
    }
    return WaitResult::Timeout;
}

[[nodiscard]] FailureClass connectSocket(
    Connection& connection,
    const Endpoint& endpoint,
    const std::chrono::milliseconds timeout
) noexcept {
    connection.close();
    const int fd = ::socket(
        endpoint.family,
        endpoint.socket_type | SOCK_NONBLOCK | SOCK_CLOEXEC,
        endpoint.protocol
    );
    if (fd < 0) {
        return FailureClass::Connect;
    }

    while (::connect(
               fd,
               reinterpret_cast<const sockaddr*>(&endpoint.address),
               endpoint.size
           ) != 0) {
        if (errno == EINTR) {
            continue;
        }
        if (errno != EINPROGRESS && errno != EALREADY) {
            (void)::close(fd);
            return FailureClass::Connect;
        }
        const auto wait = waitFd(fd, POLLOUT, Clock::now() + timeout);
        if (wait == WaitResult::Timeout) {
            (void)::close(fd);
            return FailureClass::ConnectTimeout;
        }
        if (wait != WaitResult::Ready) {
            (void)::close(fd);
            return FailureClass::Connect;
        }
        int socket_error = 0;
        socklen_t size = sizeof(socket_error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &size) != 0 ||
            socket_error != 0) {
            (void)::close(fd);
            return FailureClass::Connect;
        }
        break;
    }
    connection.fd = fd;
    return FailureClass::Count;
}

[[nodiscard]] FailureClass sendAll(
    const int fd,
    const base::ArrayView<const Byte> bytes,
    const Clock::time_point deadline
) noexcept {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto sent = ::send(
            fd,
            bytes.data() + static_cast<std::ptrdiff_t>(offset),
            bytes.size() - offset,
            MSG_NOSIGNAL
        );
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            const auto wait = waitFd(fd, POLLOUT, deadline);
            if (wait == WaitResult::Ready) {
                continue;
            }
            return wait == WaitResult::Timeout
                       ? FailureClass::RequestTimeout
                       : FailureClass::Send;
        }
        return FailureClass::Send;
    }
    return FailureClass::Count;
}

[[nodiscard]] FailureClass readExact(
    const int fd,
    const base::ArrayView<Byte> output,
    const Clock::time_point deadline
) noexcept {
    std::size_t offset = 0;
    while (offset < output.size()) {
        const auto received = ::recv(
            fd,
            output.data() + static_cast<std::ptrdiff_t>(offset),
            output.size() - offset,
            0
        );
        if (received > 0) {
            offset += static_cast<std::size_t>(received);
            continue;
        }
        if (received == 0) {
            return FailureClass::PeerClosed;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            const auto wait = waitFd(fd, POLLIN, deadline);
            if (wait == WaitResult::Ready) {
                continue;
            }
            if (wait == WaitResult::Timeout) {
                return FailureClass::RequestTimeout;
            }
            return wait == WaitResult::Closed
                       ? FailureClass::PeerClosed
                       : FailureClass::Read;
        }
        return FailureClass::Read;
    }
    return FailureClass::Count;
}

[[nodiscard]] login::LoginRequest makeRequest(
    const LoadConfig& config,
    const std::uint64_t ordinal,
    const std::uint64_t timestamp_ms
) {
    login::LoginRequest request;
    request.set_attempt_id(ordinal + 1);
    request.set_timestamp_ms(timestamp_ms);
    if (choose(config.seed, ordinal, config.attack_ratio)) {
        request.set_user_id(
            config.entity_prefix + "_attack_user_" +
            std::to_string(ordinal % config.attack_users)
        );
        request.set_ip(config.attack_ip);
        request.set_device_id(
            config.entity_prefix + "_" + config.attack_device
        );
        request.set_result(login::FAIL);
    } else {
        request.set_user_id(
            config.entity_prefix + "_normal_user_" +
            std::to_string(ordinal % config.normal_users)
        );
        const auto ip = ordinal % config.normal_ip_count;
        request.set_ip(
            "198.51." + std::to_string((ip / 256U) % 256U) + "." +
            std::to_string(ip % 256U)
        );
        request.set_device_id(
            config.entity_prefix + "_normal_device_" +
            std::to_string(ordinal % config.normal_device_count)
        );
        request.set_result(login::SUCCESS);
    }
    return request;
}

[[nodiscard]] ExchangeResult exchange(
    const int fd,
    const base::ArrayView<const Byte> request,
    const std::chrono::milliseconds timeout
) {
    const auto deadline = Clock::now() + timeout;
    auto failure = sendAll(fd, request, deadline);
    if (failure != FailureClass::Count) {
        return failedExchange(failure);
    }

    protocol::FrameHeader header{};
    failure = readExact(fd, header, deadline);
    if (failure != FailureClass::Count) {
        return failedExchange(failure);
    }
    const auto payload_size = protocol::decodePayloadLength(header);
    if (protocol::validatePayloadLength(payload_size) !=
        protocol::ProtocolError::none) {
        return failedExchange(FailureClass::Protocol);
    }

    Bytes frame(header.begin(), header.end());
    frame.resize(protocol::kFrameHeaderSize + payload_size);
    failure = readExact(
        fd,
        base::ArrayView<Byte>(frame).subview(protocol::kFrameHeaderSize),
        deadline
    );
    if (failure != FailureClass::Count) {
        return failedExchange(failure);
    }

    auto decoded = decodeLoginResponseFrame(frame);
    if (!decoded.ok()) {
        return failedExchange(
            decoded.error == LoginFrameError::ProtobufParse
                ? FailureClass::Parse
                : FailureClass::Protocol
        );
    }
    return successfulExchange(std::move(decoded.response));
}

[[nodiscard]] FailureClass classifyResponse(
    const login::LoginResponse& response,
    const std::uint64_t attempt_id
) noexcept {
    if (response.status() == login::RESPONSE_STATUS_OVERLOADED) {
        return response.has_decision()
                   ? FailureClass::Protocol
                   : FailureClass::Count;
    }
    if (response.status() == login::RESPONSE_STATUS_TIMEOUT) {
        return response.has_decision()
                   ? FailureClass::Protocol
                   : FailureClass::Count;
    }
    if (response.status() != login::RESPONSE_STATUS_OK) {
        return FailureClass::Protocol;
    }
    if (!response.has_decision() ||
        response.decision().attempt_id() != attempt_id) {
        return FailureClass::Mismatch;
    }

    const auto valid_action = [](const login::DecisionAction action) {
        return action == login::PASS || action == login::REVIEW ||
               action == login::REJECT;
    };
    if (!valid_action(response.decision().action())) {
        return FailureClass::Protocol;
    }
    for (const auto& hit : response.decision().policy_hits()) {
        if (hit.reason_code().empty() || !valid_action(hit.action())) {
            return FailureClass::Protocol;
        }
    }
    return FailureClass::Count;
}

void recordResponse(
    PhaseCounters& counters,
    const login::LoginResponse& response
) {
    switch (response.status()) {
    case login::RESPONSE_STATUS_OK:
        ++counters.service_status_counts[0];
        break;
    case login::RESPONSE_STATUS_OVERLOADED:
        ++counters.service_status_counts[1];
        return;
    case login::RESPONSE_STATUS_TIMEOUT:
        ++counters.service_status_counts[2];
        return;
    default:
        return;
    }

    switch (response.decision().action()) {
    case login::PASS:
        ++counters.decision_action_counts[0];
        break;
    case login::REVIEW:
        ++counters.decision_action_counts[1];
        break;
    case login::REJECT:
        ++counters.decision_action_counts[2];
        break;
    default:
        break;
    }
    for (const auto& hit : response.decision().policy_hits()) {
        std::size_t index = kPolicyHitNames.size() - 1;
        for (std::size_t candidate = 0;
             candidate + 1 < kPolicyHitNames.size(); ++candidate) {
            if (hit.reason_code() == kPolicyHitNames[candidate]) {
                index = candidate;
                break;
            }
        }
        ++counters.policy_hit_counts[index];
    }
}

[[nodiscard]] std::uint64_t scheduledRequests(
    const LoadConfig& config,
    const std::chrono::milliseconds duration
) noexcept {
    if (duration.count() == 0) {
        return 0;
    }
    if (config.target_qps <= 0.0) {
        return static_cast<std::uint64_t>(
            config.connection_pool_size * config.requests_per_connection
        );
    }
    return static_cast<std::uint64_t>(std::floor(
        static_cast<long double>(config.target_qps) * duration.count() /
        1000.0L
    ));
}

[[nodiscard]] Clock::time_point dueTime(
    const Clock::time_point start,
    const std::uint64_t phase_ordinal,
    const double qps
) noexcept {
    return qps <= 0.0
               ? start
               : start + scheduledOffset(phase_ordinal, qps);
}

void updateHighWatermark(
    std::atomic<std::uint64_t>& high,
    const std::uint64_t value
) noexcept {
    auto current = high.load(std::memory_order_relaxed);
    while (current < value && !high.compare_exchange_weak(
               current, value, std::memory_order_relaxed
           )) {
    }
}

void mergeCounters(PhaseCounters& target, const PhaseCounters& source) {
    target.issued_requests += source.issued_requests;
    target.decoded_responses += source.decoded_responses;
    target.failed_requests += source.failed_requests;
    target.connection_attempts += source.connection_attempts;
    target.connections_established += source.connections_established;
    target.planned_reconnects += source.planned_reconnects;
    for (std::size_t index = 0; index < target.service_status_counts.size();
         ++index) {
        target.service_status_counts[index] +=
            source.service_status_counts[index];
    }
    for (std::size_t index = 0; index < target.decision_action_counts.size();
         ++index) {
        target.decision_action_counts[index] +=
            source.decision_action_counts[index];
    }
    for (std::size_t index = 0; index < target.policy_hit_counts.size();
         ++index) {
        target.policy_hit_counts[index] += source.policy_hit_counts[index];
    }
    for (std::size_t index = 0; index < kFailureClassCount; ++index) {
        target.failures.values[index] += source.failures.values[index];
    }
    target.latencies.insert(
        target.latencies.end(), source.latencies.begin(), source.latencies.end()
    );
    target.schedule_lags.insert(
        target.schedule_lags.end(),
        source.schedule_lags.begin(),
        source.schedule_lags.end()
    );
}

[[nodiscard]] PhaseResult runPhase(
    const LoadConfig& config,
    const Endpoint& endpoint,
    const std::uint64_t first_ordinal,
    const std::uint64_t request_count,
    const std::chrono::milliseconds minimum_duration,
    std::vector<WorkerState>& states
) {
    PhaseResult phase;
    if (request_count == 0) {
        return phase;
    }

    const auto phase_start = Clock::now();
    std::atomic<std::uint64_t> next_phase_ordinal{0};
    std::atomic<std::uint64_t> outstanding{0};
    std::atomic<std::uint64_t> outstanding_high{0};
    std::vector<PhaseCounters> workers(states.size());
    std::vector<std::thread> threads;
    threads.reserve(states.size());

    for (std::size_t worker_index = 0; worker_index < states.size();
         ++worker_index) {
        threads.emplace_back([&, worker_index] {
            auto& state = states[worker_index];
            auto& counters = workers[worker_index];
            std::size_t cursor = 0;

            while (true) {
                const auto phase_ordinal = next_phase_ordinal.fetch_add(
                    1, std::memory_order_relaxed
                );
                if (phase_ordinal >= request_count) {
                    break;
                }

                const auto ordinal = first_ordinal + phase_ordinal;
                const auto due = dueTime(
                    phase_start, phase_ordinal, config.target_qps
                );
                std::this_thread::sleep_until(due);
                const auto issued_at = Clock::now();
                ++counters.issued_requests;
                counters.schedule_lags.push_back(
                    elapsedMicros(due, issued_at)
                );

                const auto active = outstanding.fetch_add(
                    1, std::memory_order_relaxed
                ) + 1;
                updateHighWatermark(outstanding_high, active);

                auto& connection =
                    state.connections[cursor++ % state.connections.size()];
                if (connection.requests >= config.requests_per_connection) {
                    connection.close();
                    ++counters.planned_reconnects;
                }

                FailureClass failure = FailureClass::Count;
                ExchangeResult exchanged;
                bool used_connection = false;
                bool reconnect_after_response = false;
                if (connection.fd < 0) {
                    ++counters.connection_attempts;
                    failure = connectSocket(
                        connection, endpoint, config.connect_timeout
                    );
                    if (failure == FailureClass::Count) {
                        ++counters.connections_established;
                    }
                }

                if (failure == FailureClass::Count) {
                    used_connection = true;
                    try {
                        const auto request = makeRequest(
                            config, ordinal, epochMillis()
                        );
                        const auto frame = encodeLoginRequestFrame(request);
                        exchanged = exchange(
                            connection.fd, frame, config.request_timeout
                        );
                        failure = exchanged.failure;
                    } catch (...) {
                        failure = FailureClass::Internal;
                    }
                    if (failure == FailureClass::Count) {
                        failure = classifyResponse(
                            exchanged.response, ordinal + 1
                        );
                        reconnect_after_response =
                            failure == FailureClass::Count &&
                            exchanged.response.status() ==
                                login::RESPONSE_STATUS_TIMEOUT;
                    }
                }

                if (used_connection) {
                    ++connection.requests;
                }
                outstanding.fetch_sub(1, std::memory_order_relaxed);

                if (failure == FailureClass::Count) {
                    ++counters.decoded_responses;
                    counters.latencies.push_back(
                        elapsedMicros(issued_at, Clock::now())
                    );
                    recordResponse(counters, exchanged.response);
                } else {
                    ++counters.failed_requests;
                    counters.failures.add(failure);
                    connection.close();
                }

                // 服务端完整写回 TIMEOUT 后关闭 Session。客户端已经把响应
                // 记为 decoded，再主动丢弃连接，避免下一次请求把预期 EOF
                // 误记为 peer_closed。
                if (reconnect_after_response) {
                    connection.close();
                    ++counters.planned_reconnects;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }
    // 限速场景的计量窗口至少覆盖配置时长。最后一个请求通常在窗口结束
    // 前一个发令间隔发出；若直接在它返回时停表，小数据集会把 QPS 严重
    // 高估。发生调度积压或响应拖尾时仍使用更长的真实完成时间。
    if (config.target_qps > 0.0) {
        std::this_thread::sleep_until(phase_start + minimum_duration);
    }
    for (const auto& worker : workers) {
        mergeCounters(phase.counters, worker);
    }
    phase.elapsed_us = elapsedMicros(phase_start, Clock::now());
    phase.max_outstanding_requests =
        outstanding_high.load(std::memory_order_relaxed);
    return phase;
}

[[nodiscard]] std::uint64_t sum(
    const std::array<std::uint64_t, 3>& values
) noexcept {
    return std::accumulate(values.begin(), values.end(), std::uint64_t{0});
}

}  // namespace

std::chrono::nanoseconds scheduledOffset(
    const std::uint64_t ordinal,
    const double target_qps
) noexcept {
    if (target_qps <= 0.0 || !std::isfinite(target_qps)) {
        return std::chrono::nanoseconds::zero();
    }
    const auto nanoseconds =
        static_cast<long double>(ordinal) * 1'000'000'000.0L / target_qps;
    const auto maximum = static_cast<long double>(
        std::numeric_limits<std::int64_t>::max()
    );
    if (nanoseconds >= maximum) {
        return std::chrono::nanoseconds::max();
    }
    return std::chrono::nanoseconds(
        static_cast<std::int64_t>(nanoseconds)
    );
}

LoadConfigValidation validateLoadConfig(const LoadConfig& config) {
    const auto invalid = [](std::string message) {
        return LoadConfigValidation{false, std::move(message)};
    };
    if (config.host.empty() || containsWhitespace(config.host)) {
        return invalid("host 不能为空或包含空白");
    }
    if (config.port == 0) {
        return invalid("port 必须大于 0");
    }
    if (config.request_concurrency == 0 ||
        config.connection_pool_size == 0) {
        return invalid(
            "request_concurrency 和 connection_pool_size 必须大于 0"
        );
    }
    if (config.request_concurrency > config.connection_pool_size) {
        return invalid(
            "request_concurrency 不得超过 connection_pool_size"
        );
    }
    if (config.requests_per_connection == 0) {
        return invalid("requests_per_connection 必须大于 0");
    }
    if (config.connection_pool_size >
        std::numeric_limits<std::size_t>::max() /
            config.requests_per_connection) {
        return invalid("不限速请求数超出 size_t");
    }
    if (!std::isfinite(config.target_qps) || config.target_qps < 0.0) {
        return invalid("target_qps 必须是非负有限数");
    }
    if (config.duration.count() <= 0 || config.warmup.count() < 0 ||
        config.connect_timeout.count() <= 0 ||
        config.request_timeout.count() <= 0) {
        return invalid("时长和超时配置无效");
    }
    if (!std::isfinite(config.attack_ratio) || config.attack_ratio < 0.0 ||
        config.attack_ratio > 1.0) {
        return invalid("attack_ratio 必须位于 [0,1]");
    }
    if (config.normal_users == 0 || config.normal_ip_count == 0 ||
        config.normal_device_count == 0 || config.attack_users == 0) {
        return invalid("负载基数必须大于 0");
    }
    if (config.entity_prefix.empty() || containsWhitespace(config.entity_prefix) ||
        config.entity_prefix.size() > 64) {
        return invalid("entity_prefix 必须为不超过 64 字节的单个 token");
    }
    if (config.attack_device.empty() || containsWhitespace(config.attack_device) ||
        config.attack_device.size() > 48) {
        return invalid("attack_device 必须为不超过 48 字节的单个 token");
    }
    if (!isIpAddress(config.attack_ip)) {
        return invalid("attack_ip 必须是合法 IPv4 或 IPv6 地址");
    }

    const auto measured_requests = static_cast<long double>(config.target_qps) *
                                   config.duration.count() / 1000.0L;
    const auto warmup_requests = static_cast<long double>(config.target_qps) *
                                 config.warmup.count() / 1000.0L;
    const auto maximum = static_cast<long double>(
        std::numeric_limits<std::uint64_t>::max()
    );
    if (config.target_qps > 0.0 &&
        (measured_requests >= maximum || warmup_requests >= maximum ||
         measured_requests + warmup_requests >= maximum)) {
        return invalid("计划请求数超出 uint64 范围");
    }
    const auto measured_count = scheduledRequests(config, config.duration);
    const auto warmup_count = scheduledRequests(config, config.warmup);
    if (measured_count == 0) {
        return invalid("计量阶段至少需要一个请求");
    }
    if (warmup_count > std::numeric_limits<std::uint64_t>::max() -
                           measured_count) {
        return invalid("预热与计量请求总数超出 uint64 范围");
    }
    return {};
}

std::string_view failureClassName(const FailureClass failure) noexcept {
    static constexpr std::array<std::string_view, kFailureClassCount> names{
        "connect",
        "connect_timeout",
        "send",
        "read",
        "peer_closed",
        "protocol",
        "parse",
        "mismatch",
        "request_timeout",
        "internal",
    };
    const auto index = static_cast<std::size_t>(failure);
    return index < names.size() ? names[index] : "unknown";
}

void FailureCounters::add(
    const FailureClass failure,
    const std::uint64_t count
) noexcept {
    const auto index = static_cast<std::size_t>(failure);
    if (index < values.size()) {
        values[index] += count;
    }
}

std::uint64_t FailureCounters::get(const FailureClass failure) const noexcept {
    const auto index = static_cast<std::size_t>(failure);
    return index < values.size() ? values[index] : 0;
}

std::uint64_t FailureCounters::total() const noexcept {
    return std::accumulate(values.begin(), values.end(), std::uint64_t{0});
}

bool BenchmarkMetrics::accountingConsistent() const noexcept {
    return issued_requests == decoded_responses + failed_requests &&
           decoded_responses == sum(service_status_counts) &&
           service_status_counts[0] == sum(decision_action_counts) &&
           failed_requests == failures.total();
}

bool BenchmarkResult::requestAccountingConsistent() const noexcept {
    return metrics.expected_requests == metrics.issued_requests &&
           metrics.accountingConsistent();
}

LatencySummary summarizeLatencies(
    const base::ArrayView<const std::uint64_t> values
) {
    LatencySummary result;
    if (values.empty()) {
        return result;
    }

    std::vector<std::uint64_t> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&sorted](const double ratio) {
        const auto index = static_cast<std::size_t>(std::ceil(
            ratio * static_cast<double>(sorted.size())
        )) - 1;
        return sorted[std::min(index, sorted.size() - 1)];
    };

    long double total = 0.0L;
    for (const auto value : sorted) {
        total += value;
    }
    result.count = sorted.size();
    result.average_us = static_cast<double>(total / sorted.size());
    result.p50_us = percentile(0.50);
    result.p95_us = percentile(0.95);
    result.p99_us = percentile(0.99);
    result.max_us = sorted.back();
    return result;
}

double calculateQps(
    const std::uint64_t decoded_responses,
    const std::uint64_t measurement_us
) noexcept {
    return measurement_us == 0
               ? 0.0
               : static_cast<double>(decoded_responses) * 1'000'000.0 /
                     static_cast<double>(measurement_us);
}

BenchmarkResult runBenchmark(const LoadConfig& config) {
    const auto validation = validateLoadConfig(config);
    if (!validation.ok()) {
        throw std::invalid_argument(validation.message);
    }

    const auto endpoint = resolveEndpoint(config);
    const auto warmup_requests = scheduledRequests(config, config.warmup);
    const auto measured_requests = scheduledRequests(config, config.duration);

    std::vector<WorkerState> states(config.request_concurrency);
    for (std::size_t index = 0; index < config.connection_pool_size; ++index) {
        states[index % states.size()].connections.emplace_back();
    }

    BenchmarkResult result;
    result.config = config;
    result.started_at_epoch_ms = epochMillis();

    // 预热线程全部 join 后才创建计量阶段时钟。这样不存在“一个 worker
    // 仍在预热、另一个 worker 已计量”的重叠，也不会把预热尾部算入 QPS。
    const auto warmup = runPhase(
        config, endpoint, 0, warmup_requests, config.warmup, states
    );
    const auto measured = runPhase(
        config,
        endpoint,
        warmup_requests,
        measured_requests,
        config.duration,
        states
    );

    result.measurement_us = measured.elapsed_us;
    auto& metrics = result.metrics;
    metrics.expected_requests = measured_requests;
    metrics.issued_requests = measured.counters.issued_requests;
    metrics.decoded_responses = measured.counters.decoded_responses;
    metrics.failed_requests = measured.counters.failed_requests;
    metrics.warmup_issued_requests = warmup.counters.issued_requests;
    metrics.warmup_decoded_responses = warmup.counters.decoded_responses;
    metrics.warmup_failed_requests = warmup.counters.failed_requests;
    metrics.connection_attempts = warmup.counters.connection_attempts +
                                  measured.counters.connection_attempts;
    metrics.connections_established =
        warmup.counters.connections_established +
        measured.counters.connections_established;
    metrics.planned_reconnects = warmup.counters.planned_reconnects +
                                 measured.counters.planned_reconnects;
    metrics.max_outstanding_requests = std::max(
        warmup.max_outstanding_requests,
        measured.max_outstanding_requests
    );
    metrics.service_status_counts = measured.counters.service_status_counts;
    metrics.decision_action_counts = measured.counters.decision_action_counts;
    metrics.policy_hit_counts = measured.counters.policy_hit_counts;
    metrics.failures = measured.counters.failures;
    metrics.qps = calculateQps(metrics.decoded_responses, result.measurement_us);
    metrics.failure_rate = metrics.issued_requests == 0
                               ? 0.0
                               : static_cast<double>(metrics.failed_requests) /
                                     metrics.issued_requests;
    metrics.latency = summarizeLatencies(measured.counters.latencies);
    metrics.schedule_lag = summarizeLatencies(measured.counters.schedule_lags);
    return result;
}

std::string renderKeyValueSummary(const BenchmarkResult& result) {
    const auto& config = result.config;
    const auto& metrics = result.metrics;
    std::ostringstream output;
    output << std::fixed << std::setprecision(3)
           << "expected_requests=" << metrics.expected_requests
           << " issued_requests=" << metrics.issued_requests
           << " decoded_responses=" << metrics.decoded_responses
           << " failed_requests=" << metrics.failed_requests
           << " warmup_issued_requests=" << metrics.warmup_issued_requests
           << " warmup_decoded_responses="
           << metrics.warmup_decoded_responses
           << " warmup_failed_requests=" << metrics.warmup_failed_requests
           << " status_ok=" << metrics.service_status_counts[0]
           << " status_overloaded=" << metrics.service_status_counts[1]
           << " status_timeout=" << metrics.service_status_counts[2]
           << " action_pass=" << metrics.decision_action_counts[0]
           << " action_review=" << metrics.decision_action_counts[1]
           << " action_reject=" << metrics.decision_action_counts[2];
    for (std::size_t index = 0; index < kPolicyHitNames.size(); ++index) {
        output << " policy_" << kPolicyHitNames[index] << '='
               << metrics.policy_hit_counts[index];
    }
    for (std::size_t index = 0; index < kFailureClassCount; ++index) {
        output << " failure_"
               << failureClassName(static_cast<FailureClass>(index)) << '='
               << metrics.failures.values[index];
    }
    output << " qps=" << metrics.qps
           << " failure_rate=" << metrics.failure_rate
           << " latency_count=" << metrics.latency.count
           << " latency_average_us=" << metrics.latency.average_us
           << " p50_us=" << metrics.latency.p50_us
           << " p95_us=" << metrics.latency.p95_us
           << " p99_us=" << metrics.latency.p99_us
           << " max_us=" << metrics.latency.max_us
           << " schedule_lag_p95_us=" << metrics.schedule_lag.p95_us
           << " measurement_us=" << result.measurement_us
           << " connection_attempts=" << metrics.connection_attempts
           << " connections_established="
           << metrics.connections_established
           << " planned_reconnects=" << metrics.planned_reconnects
           << " max_outstanding_requests="
           << metrics.max_outstanding_requests
           << " request_concurrency=" << config.request_concurrency
           << " connection_pool_size=" << config.connection_pool_size
           << " requests_per_connection="
           << config.requests_per_connection
           << " target_qps=" << config.target_qps
           << " warmup_ms=" << config.warmup.count()
           << " duration_ms=" << config.duration.count()
           << " attack_ratio=" << config.attack_ratio
           << " seed=" << config.seed
           << " entity_prefix=" << config.entity_prefix
           << " attack_ip=" << config.attack_ip;
    return output.str();
}

}  // namespace aegisflow::benchmark
