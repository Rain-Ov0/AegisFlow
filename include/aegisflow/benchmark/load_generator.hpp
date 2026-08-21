#pragma once

#include "aegisflow/base/array_view.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace aegisflow::benchmark {

// 原生协议 benchmark 只保留复现实验所需的负载参数。
struct LoadConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 8080;
    std::size_t request_concurrency = 2;
    std::size_t connection_pool_size = 16;
    std::size_t requests_per_connection = 1000;
    double target_qps = 1000.0;
    std::chrono::milliseconds warmup{1000};
    std::chrono::milliseconds duration{10000};
    std::chrono::milliseconds connect_timeout{3000};
    std::chrono::milliseconds request_timeout{3000};
    std::uint64_t seed = 20260815;
    double attack_ratio = 0.20;
    std::size_t normal_users = 100000;
    std::size_t normal_ip_count = 1000;
    std::size_t normal_device_count = 5000;
    std::size_t attack_users = 100;
    std::string attack_ip = "203.0.113.10";
    std::string attack_device = "attack_device";
    std::string entity_prefix = "run";
};

struct LoadConfigValidation {
    bool valid = true;
    std::string message;
    [[nodiscard]] bool ok() const noexcept { return valid; }
};

[[nodiscard]] LoadConfigValidation validateLoadConfig(const LoadConfig& config);

enum class FailureClass : std::uint8_t {
    Connect,
    ConnectTimeout,
    Send,
    Read,
    PeerClosed,
    Protocol,
    Parse,
    Mismatch,
    RequestTimeout,
    Internal,
    Count,
};

inline constexpr std::size_t kFailureClassCount =
    static_cast<std::size_t>(FailureClass::Count);

[[nodiscard]] std::string_view failureClassName(FailureClass failure) noexcept;

struct FailureCounters {
    std::array<std::uint64_t, kFailureClassCount> values{};
    void add(FailureClass failure, std::uint64_t count = 1) noexcept;
    [[nodiscard]] std::uint64_t get(FailureClass failure) const noexcept;
    [[nodiscard]] std::uint64_t total() const noexcept;
};

struct LatencySummary {
    std::uint64_t count = 0;
    double average_us = 0.0;
    std::uint64_t p50_us = 0;
    std::uint64_t p95_us = 0;
    std::uint64_t p99_us = 0;
    std::uint64_t max_us = 0;
};

inline constexpr std::array<std::string_view, 3> kServiceStatusNames = {
    "ok", "overloaded", "timeout"};
inline constexpr std::array<std::string_view, 3> kDecisionActionNames = {
    "pass", "review", "reject"};
inline constexpr std::array<std::string_view, 8> kPolicyHitNames = {
    "blacklisted_user",
    "blacklisted_ip",
    "blacklisted_device",
    "credential_stuffing_attack",
    "too_many_failed_login",
    "ip_many_users_failed_login",
    "device_many_accounts",
    "other",
};

struct BenchmarkMetrics {
    std::uint64_t expected_requests = 0;
    std::uint64_t issued_requests = 0;
    std::uint64_t decoded_responses = 0;
    std::uint64_t failed_requests = 0;
    std::uint64_t warmup_issued_requests = 0;
    std::uint64_t warmup_decoded_responses = 0;
    std::uint64_t warmup_failed_requests = 0;
    std::uint64_t connection_attempts = 0;
    std::uint64_t connections_established = 0;
    std::uint64_t planned_reconnects = 0;
    std::uint64_t max_outstanding_requests = 0;
    std::array<std::uint64_t, 3> service_status_counts{};
    std::array<std::uint64_t, 3> decision_action_counts{};
    std::array<std::uint64_t, 8> policy_hit_counts{};
    double qps = 0.0;
    double failure_rate = 0.0;
    FailureCounters failures;
    LatencySummary latency;
    LatencySummary schedule_lag;

    // 所有已发请求都必须进入“已解码响应”或“失败”终态；只有 OK
    // 响应携带领域动作，因此第三条等式只核对 status_ok。
    [[nodiscard]] bool accountingConsistent() const noexcept;
};

struct BenchmarkResult {
    LoadConfig config;
    std::uint64_t started_at_epoch_ms = 0;
    std::uint64_t measurement_us = 0;
    BenchmarkMetrics metrics;

    [[nodiscard]] bool requestAccountingConsistent() const noexcept;
};

[[nodiscard]] std::chrono::nanoseconds scheduledOffset(
    std::uint64_t ordinal,
    double target_qps
) noexcept;

[[nodiscard]] LatencySummary summarizeLatencies(
    base::ArrayView<const std::uint64_t> values
);

[[nodiscard]] double calculateQps(
    std::uint64_t decoded_responses,
    std::uint64_t measurement_us
) noexcept;

[[nodiscard]] BenchmarkResult runBenchmark(const LoadConfig& config);
[[nodiscard]] std::string renderKeyValueSummary(const BenchmarkResult& result);

}  // namespace aegisflow::benchmark
