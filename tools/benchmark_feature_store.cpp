#include "aegisflow/app/login_request_validator.hpp"
#include "aegisflow/feature/feature_store.hpp"

#include "login.pb.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using aegisflow::feature::FeatureStoreStats;
using aegisflow::feature::LoginFeatureSnapshot;
using aegisflow::feature::LoginFeatureStore;

struct Options {
    std::size_t events = 10'000;
    std::size_t warmup_events = 1'000;
    std::size_t rounds = 3;
    std::size_t threads = 1;
};

enum class Scenario { ColdNormal, HotNormal, ColdAttack, HotAttack };

struct LatencyStats {
    std::uint64_t p50_ns = 0;
    std::uint64_t p95_ns = 0;
    std::uint64_t p99_ns = 0;
    std::uint64_t max_ns = 0;
};

struct RoundResult {
    double qps = 0.0;
    LatencyStats latency;
    FeatureStoreStats states;
    LoginFeatureSnapshot snapshot;
};

void printUsage(const char* program) {
    std::cout
        << "Usage: " << program << " [options]\n"
        << "  --events N          measured events per scenario, default 10000\n"
        << "  --warmup-events N   warmup events per scenario, default 1000\n"
        << "  --rounds N          independent rounds, default 3\n"
        << "  --threads 1         explicit single-thread mode\n"
        << "  --help              show this help\n";
}

std::size_t parseSize(const char* value, const std::string_view name) {
    std::size_t parsed_chars = 0;
    const auto parsed = std::stoull(value, &parsed_chars);
    if (parsed_chars != std::string_view(value).size()) {
        throw std::invalid_argument(std::string(name) + " 含有非整数字符");
    }
    return static_cast<std::size_t>(parsed);
}

bool parseOptions(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        const auto value = [&](const std::string_view name) {
            if (++index >= argc) {
                throw std::invalid_argument(std::string(name) + " 缺少参数");
            }
            return argv[index];
        };
        if (argument == "--events") {
            options.events = parseSize(value(argument), argument);
        } else if (argument == "--warmup-events") {
            options.warmup_events = parseSize(value(argument), argument);
        } else if (argument == "--rounds") {
            options.rounds = parseSize(value(argument), argument);
        } else if (argument == "--threads") {
            options.threads = parseSize(value(argument), argument);
        } else if (argument == "--help") {
            printUsage(argv[0]);
            return false;
        } else {
            throw std::invalid_argument("未知参数: " + std::string(argument));
        }
    }
    if (options.events == 0 || options.rounds == 0) {
        throw std::invalid_argument("events 和 rounds 必须大于 0");
    }
    if (options.threads != 1) {
        throw std::invalid_argument("该微基准只支持 --threads 1");
    }
    return true;
}

std::string_view scenarioName(const Scenario scenario) noexcept {
    switch (scenario) {
        case Scenario::ColdNormal: return "cold_normal";
        case Scenario::HotNormal: return "hot_normal";
        case Scenario::ColdAttack: return "cold_attack";
        case Scenario::HotAttack: return "hot_attack";
    }
    return "unknown";
}

std::string makeIp(const std::size_t index) {
    return "10." + std::to_string((index / 65'536) % 256) + "." +
           std::to_string((index / 256) % 256) + "." +
           std::to_string(index % 256);
}

aegisflow::domain::LoginAttempt makeAttempt(
    const Scenario scenario,
    const std::uint64_t ordinal,
    const std::uint64_t timestamp_ms
) {
    const bool cold = scenario == Scenario::ColdNormal ||
                      scenario == Scenario::ColdAttack;
    const bool attack = scenario == Scenario::ColdAttack ||
                        scenario == Scenario::HotAttack;
    const auto key = cold ? ordinal : ordinal % 100;

    aegisflow::login::LoginRequest request;
    request.set_attempt_id(ordinal + 1);
    request.set_timestamp_ms(timestamp_ms);
    request.set_user_id(
        std::string(attack ? "attack_user_" : "normal_user_") +
        std::to_string(key));
    request.set_ip(attack ? "203.0.113.10"
                          : makeIp(cold ? ordinal : ordinal % 32));
    request.set_device_id(
        attack ? "attack_device"
               : std::string("normal_device_") +
                     std::to_string(cold ? ordinal : ordinal % 64));
    request.set_result(
        attack || ordinal % 10 == 0
            ? aegisflow::login::FAIL
            : aegisflow::login::SUCCESS);

    auto attempt = aegisflow::app::LoginRequestValidator::validate(
        request, timestamp_ms);
    if (!attempt.has_value()) {
        throw std::runtime_error("微基准样本未通过登录校验");
    }
    return std::move(*attempt);
}

std::uint64_t percentile(
    const std::vector<std::uint64_t>& sorted,
    const double fraction
) {
    const auto index = static_cast<std::size_t>(
        static_cast<double>(sorted.size() - 1) * fraction);
    return sorted[index];
}

LatencyStats summarize(std::vector<std::uint64_t> samples) {
    std::sort(samples.begin(), samples.end());
    LatencyStats result;
    result.p50_ns = percentile(samples, 0.50);
    result.p95_ns = percentile(samples, 0.95);
    result.p99_ns = percentile(samples, 0.99);
    result.max_ns = samples.back();
    return result;
}

RoundResult runRound(
    const Scenario scenario,
    const Options& options,
    const std::size_t round
) {
    LoginFeatureStore store;
    constexpr std::uint64_t kBaseTimestampMs = 2'000'000'000;
    const auto round_offset = static_cast<std::uint64_t>(round) *
                              (options.warmup_events + options.events + 1);

    for (std::size_t index = 0; index < options.warmup_events; ++index) {
        const auto ordinal = round_offset + index;
        const auto timestamp = kBaseTimestampMs + ordinal;
        const auto attempt = makeAttempt(scenario, ordinal, timestamp);
        (void)store.updateAndGet(attempt, timestamp);
    }

    std::vector<std::uint64_t> latency_ns;
    latency_ns.reserve(options.events);
    std::uint64_t measured_ns = 0;
    LoginFeatureSnapshot snapshot;
    for (std::size_t index = 0; index < options.events; ++index) {
        const auto ordinal = round_offset + options.warmup_events + index;
        const auto timestamp = kBaseTimestampMs + ordinal;
        const auto attempt = makeAttempt(scenario, ordinal, timestamp);
        const auto started = std::chrono::steady_clock::now();
        snapshot = store.updateAndGet(attempt, timestamp);
        const auto elapsed = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started).count());
        latency_ns.push_back(elapsed);
        measured_ns += elapsed;
    }

    RoundResult result;
    result.qps = measured_ns == 0
                     ? 0.0
                     : static_cast<double>(options.events) *
                           1'000'000'000.0 /
                           static_cast<double>(measured_ns);
    result.latency = summarize(std::move(latency_ns));
    result.states = store.currentStats();
    result.snapshot = snapshot;
    return result;
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

std::uint64_t median(std::vector<std::uint64_t> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

void printResult(
    const std::string_view scope,
    const Scenario scenario,
    const std::size_t round,
    const Options& options,
    const RoundResult& result
) {
    std::cout << "scope=" << scope
              << " scenario=" << scenarioName(scenario)
              << " round=" << round
              << " threads=1"
              << " warmup_events=" << options.warmup_events
              << " events=" << options.events
              << " qps=" << std::fixed << std::setprecision(2) << result.qps
              << " p50_us=" << result.latency.p50_ns / 1000.0
              << " p95_us=" << result.latency.p95_ns / 1000.0
              << " p99_us=" << result.latency.p99_ns / 1000.0
              << " max_us=" << result.latency.max_ns / 1000.0
              << " user_states=" << result.states.user_state_count
              << " ip_states=" << result.states.ip_state_count
              << " device_states=" << result.states.device_state_count
              << " ip_members=" << result.states.ip_distinct_member_count
              << " device_members="
              << result.states.device_distinct_member_count
              << " snapshot_user_fail_5m="
              << result.snapshot.user_login_fail_5m
              << " snapshot_ip_failed_users_10m="
              << result.snapshot.ip_distinct_failed_user_10m
              << " snapshot_ip_failures_10m="
              << result.snapshot.ip_login_failures_10m
              << " snapshot_device_accounts_10m="
              << result.snapshot.device_distinct_account_10m
              << " distinct_max_members_per_key="
              << LoginFeatureStore::kDistinctMaxMembers << '\n';
}

RoundResult medianResult(const std::vector<RoundResult>& rounds) {
    std::vector<double> qps;
    std::vector<std::uint64_t> p50;
    std::vector<std::uint64_t> p95;
    std::vector<std::uint64_t> p99;
    std::vector<std::uint64_t> maximum;
    for (const auto& round : rounds) {
        qps.push_back(round.qps);
        p50.push_back(round.latency.p50_ns);
        p95.push_back(round.latency.p95_ns);
        p99.push_back(round.latency.p99_ns);
        maximum.push_back(round.latency.max_ns);
    }
    auto result = rounds.back();
    result.qps = median(std::move(qps));
    result.latency.p50_ns = median(std::move(p50));
    result.latency.p95_ns = median(std::move(p95));
    result.latency.p99_ns = median(std::move(p99));
    result.latency.max_ns = median(std::move(maximum));
    return result;
}

}  // 命名空间

int main(int argc, char** argv) {
    Options options;
    try {
        if (!parseOptions(argc, argv, options)) {
            return 0;
        }
        constexpr std::array scenarios = {
            Scenario::ColdNormal,
            Scenario::HotNormal,
            Scenario::ColdAttack,
            Scenario::HotAttack,
        };
        for (const auto scenario : scenarios) {
            std::vector<RoundResult> results;
            results.reserve(options.rounds);
            for (std::size_t round = 1; round <= options.rounds; ++round) {
                results.push_back(runRound(scenario, options, round));
                printResult("round", scenario, round, options, results.back());
            }
            printResult(
                "median", scenario, 0, options, medianResult(results));
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark_feature_store: " << error.what() << '\n';
        printUsage(argv[0]);
        return 1;
    }
}
