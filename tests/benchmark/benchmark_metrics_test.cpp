#include "aegisflow/benchmark/load_generator.hpp"

#include "tests/support/test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using aegisflow::test::require;
using namespace std::chrono_literals;

void loadConfigurationRejectsImpossibleOrEmptyRuns() {
    aegisflow::benchmark::LoadConfig config;
    config.request_concurrency = 4;
    config.connection_pool_size = 3;
    require(
        !aegisflow::benchmark::validateLoadConfig(config).ok(),
        "请求并发超过连接池时必须拒绝配置"
    );

    config.connection_pool_size = 4;
    require(
        aegisflow::benchmark::validateLoadConfig(config).ok(),
        "语义一致的 benchmark 配置必须通过校验"
    );

    config.target_qps = 0.001;
    config.duration = 1ms;
    require(
        !aegisflow::benchmark::validateLoadConfig(config).ok(),
        "计量阶段取整后没有请求时必须拒绝配置"
    );

    config = {};
    config.attack_ip = "not-an-ip";
    require(
        !aegisflow::benchmark::validateLoadConfig(config).ok(),
        "非法攻击 IP 不得进入负载"
    );
}

void schedulerUsesOrdinalAndTargetRate() {
    require(
        aegisflow::benchmark::scheduledOffset(5, 1000.0) == 5ms,
        "定速器必须按 phase ordinal 和目标 QPS 计算计划偏移"
    );
    require(
        aegisflow::benchmark::scheduledOffset(99, 0.0) == 0ns,
        "不限速模式不得引入计划等待"
    );
}

aegisflow::benchmark::BenchmarkMetrics consistentMetrics() {
    aegisflow::benchmark::BenchmarkMetrics metrics;
    metrics.expected_requests = 10;
    metrics.issued_requests = 10;
    metrics.decoded_responses = 8;
    metrics.failed_requests = 2;
    metrics.service_status_counts = {6, 1, 1};
    metrics.decision_action_counts = {3, 2, 1};
    metrics.failures.add(aegisflow::benchmark::FailureClass::Connect, 2);
    return metrics;
}

void allAccountingEquationsAreEnforced() {
    auto metrics = consistentMetrics();
    require(metrics.accountingConsistent(), "四项终态记账必须一致");

    ++metrics.failed_requests;
    require(
        !metrics.accountingConsistent(),
        "issued 必须等于 decoded 与 failed 之和"
    );
    --metrics.failed_requests;

    ++metrics.service_status_counts[2];
    require(
        !metrics.accountingConsistent(),
        "decoded 必须等于三个 status 之和"
    );
    --metrics.service_status_counts[2];

    ++metrics.decision_action_counts[2];
    require(
        !metrics.accountingConsistent(),
        "status_ok 必须等于三个 action 之和"
    );
    --metrics.decision_action_counts[2];

    metrics.failures.add(aegisflow::benchmark::FailureClass::Read);
    require(
        !metrics.accountingConsistent(),
        "failed 必须等于分类失败计数之和"
    );
}

void qpsAndNearestRankPercentilesUseMeasuredValues() {
    require(
        aegisflow::benchmark::calculateQps(250, 2'000'000) == 125.0,
        "QPS 必须使用 decoded_responses / measurement_seconds"
    );
    require(
        aegisflow::benchmark::calculateQps(10, 0) == 0.0,
        "零计量时长不得除零"
    );

    const std::vector<std::uint64_t> values = {50, 10, 40, 20, 30};
    const auto summary = aegisflow::benchmark::summarizeLatencies(values);
    require(
        summary.count == 5 && summary.average_us == 30.0 &&
            summary.p50_us == 30 && summary.p95_us == 50 &&
            summary.p99_us == 50 && summary.max_us == 50,
        "小数据集分位数必须使用 nearest-rank 且结果可复现"
    );
}

void keyValueSummaryIsStableAndShellReadable() {
    aegisflow::benchmark::BenchmarkResult result;
    result.config.request_concurrency = 4;
    result.config.connection_pool_size = 4;
    result.config.entity_prefix = "round_7";
    result.config.attack_ip = "2001:db8::7";
    result.measurement_us = 2'000'000;
    result.metrics = consistentMetrics();
    result.metrics.qps = 4.0;
    result.metrics.latency.count = 8;
    result.metrics.latency.p50_us = 10;
    result.metrics.latency.p95_us = 20;
    result.metrics.latency.p99_us = 30;
    result.metrics.latency.max_us = 40;

    const auto summary = aegisflow::benchmark::renderKeyValueSummary(result);
    require(
        summary.find("issued_requests=10") != std::string::npos &&
            summary.find("decoded_responses=8") != std::string::npos &&
            summary.find("failed_requests=2") != std::string::npos &&
            summary.find("status_ok=6") != std::string::npos &&
            summary.find("action_reject=1") != std::string::npos &&
            summary.find("failure_connect=2") != std::string::npos &&
            summary.find("qps=4.000") != std::string::npos &&
            summary.find("p99_us=30") != std::string::npos &&
            summary.find("entity_prefix=round_7") != std::string::npos,
        "终端摘要必须提供稳定 key=value 指标"
    );
    require(
        summary.find('{') == std::string::npos &&
            summary.find('"') == std::string::npos &&
            summary.find(',') == std::string::npos,
        "终端摘要不得退回 JSON 或 CSV"
    );
}

}  // namespace

int main() {
    return aegisflow::test::runModule(
        "benchmark_metrics",
        {
            {"负载配置", loadConfigurationRejectsImpossibleOrEmptyRuns},
            {"定速调度", schedulerUsesOrdinalAndTargetRate},
            {"请求记账", allAccountingEquationsAreEnforced},
            {"QPS 与分位数", qpsAndNearestRankPercentilesUseMeasuredValues},
            {"key=value 摘要", keyValueSummaryIsStableAndShellReadable},
        }
    );
}
