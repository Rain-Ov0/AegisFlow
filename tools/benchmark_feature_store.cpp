#include "aegisflow/feature/feature_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace v1 = aegisflow::v1;

using aegisflow::feature::FeatureSnapshot;
using aegisflow::feature::FeatureStore;

struct Options {
    size_t normal_events = 10000;
    size_t attack_events = 1000;
    size_t normal_ip_num = 1000;
    size_t normal_device_num = 5000;
};

struct LatencyStats {
    uint64_t p50_us = 0;
    uint64_t p95_us = 0;
    uint64_t p99_us = 0;
    uint64_t max_us = 0;
};

void printUsage(const char* program) {
    std::cout
        << "Usage: " << program << " [options]\n"
        << "Options:\n"
        << "  --normal-events N       normal traffic event count, default 10000\n"
        << "  --attack-events N       attack traffic event count, default 1000\n"
        << "  --normal-ip-num N       normal traffic IP count, default 1000\n"
        << "  --normal-device-num N   normal traffic device count, default 5000\n"
        << "  --help                  show this help\n";
}

size_t readSize(const char* value) {
    const auto parsed = std::stoull(value);
    return static_cast<size_t>(parsed);
}

bool parseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        auto requireValue = [&](const std::string& name) -> const char* {
            if (i + 1 >= argc) {
                throw std::invalid_argument("missing value for " + name);
            }
            return argv[++i];
        };

        if (arg == "--normal-events") {
            options.normal_events = readSize(requireValue(arg));
        } else if (arg == "--attack-events") {
            options.attack_events = readSize(requireValue(arg));
        } else if (arg == "--normal-ip-num") {
            options.normal_ip_num = readSize(requireValue(arg));
        } else if (arg == "--normal-device-num") {
            options.normal_device_num = readSize(requireValue(arg));
        } else if (arg == "--help") {
            printUsage(argv[0]);
            return false;
        } else {
            throw std::invalid_argument("unknown option: " + arg);
        }
    }

    if (options.normal_ip_num == 0) {
        throw std::invalid_argument("normal_ip_num must be greater than 0");
    }

    if (options.normal_device_num == 0) {
        throw std::invalid_argument("normal_device_num must be greater than 0");
    }

    return true;
}

v1::Event makeEvent(
    uint64_t event_id,
    const std::string& user_id,
    uint64_t timestamp_ms,
    v1::EventType type,
    v1::EventResult result,
    const std::string& ip,
    const std::string& device_id,
    const std::string& scene
) {
    v1::Event event;
    event.set_event_id(event_id);
    event.set_user_id(user_id);
    event.set_timestamp_ms(timestamp_ms);
    event.set_type(type);
    event.set_result(result);
    event.set_ip(ip);
    event.set_device_id(device_id);
    event.set_scene(scene);
    return event;
}

uint64_t elapsedMicros(std::chrono::steady_clock::time_point start) {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now() - start).count()
    );
}

LatencyStats buildLatencyStats(std::vector<uint64_t> samples) {
    LatencyStats stats;

    if (samples.empty()) {
        return stats;
    }

    std::sort(samples.begin(), samples.end());

    auto percentile = [&](double p) {
        const auto index = static_cast<size_t>(
            (samples.size() - 1) * p
        );
        return samples[index];
    };

    stats.p50_us = percentile(0.50);
    stats.p95_us = percentile(0.95);
    stats.p99_us = percentile(0.99);
    stats.max_us = samples.back();

    return stats;
}

int main(int argc, char** argv) {
    Options options;

    try {
        if (!parseOptions(argc, argv, options)) {
            return 0;
        }
    } catch (const std::exception& ex) {
        std::cerr << "invalid options: " << ex.what() << std::endl;
        printUsage(argv[0]);
        return 1;
    }

    FeatureStore store;
    std::vector<uint64_t> latencies_us;
    latencies_us.reserve(options.normal_events + options.attack_events);

    constexpr uint64_t kBaseTsMs = 1000000000;
    const auto bench_start = std::chrono::steady_clock::now();

    FeatureSnapshot normal_snapshot;

    for (size_t i = 0; i < options.normal_events; ++i) {
        const uint64_t ts_ms = kBaseTsMs + i;
        const auto start = std::chrono::steady_clock::now();

        normal_snapshot = store.updateAndGet(
            makeEvent(
                i + 1,
                "normal_user_" + std::to_string(i),
                ts_ms,
                v1::LOGIN,
                v1::SUCCESS,
                "198.51.100." + std::to_string(i % options.normal_ip_num),
                "normal_device_" + std::to_string(i % options.normal_device_num),
                "login"
            ),
            ts_ms
        );

        latencies_us.push_back(elapsedMicros(start));
    }

    FeatureSnapshot attack_snapshot;
    const std::string attack_ip = "203.0.113.10";
    const std::string attack_device = "attack_device";

    for (size_t i = 0; i < options.attack_events; ++i) {
        const uint64_t event_id = options.normal_events + i + 1;
        const uint64_t ts_ms = kBaseTsMs + options.normal_events + i;
        const auto start = std::chrono::steady_clock::now();

        attack_snapshot = store.updateAndGet(
            makeEvent(
                event_id,
                "attack_user_" + std::to_string(i),
                ts_ms,
                v1::LOGIN,
                v1::FAIL,
                attack_ip,
                attack_device,
                "login"
            ),
            ts_ms
        );

        latencies_us.push_back(elapsedMicros(start));
    }

    const uint64_t total_us = elapsedMicros(bench_start);
    const auto stats = buildLatencyStats(latencies_us);

    const size_t total_events = options.normal_events + options.attack_events;
    const double qps = total_us == 0
        ? 0.0
        : static_cast<double>(total_events) * 1000000.0 / static_cast<double>(total_us);

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "events=" << total_events << '\n';
    std::cout << "normal_events=" << options.normal_events << '\n';
    std::cout << "attack_events=" << options.attack_events << '\n';
    std::cout << "total_us=" << total_us << '\n';
    std::cout << "qps=" << qps << '\n';
    std::cout << "latency_p50_us=" << stats.p50_us << '\n';
    std::cout << "latency_p95_us=" << stats.p95_us << '\n';
    std::cout << "latency_p99_us=" << stats.p99_us << '\n';
    std::cout << "latency_max_us=" << stats.max_us << '\n';

    std::cout << "attack_ip_distinct_user_10m="
              << attack_snapshot.ip_distinct_user_10m << '\n';
    std::cout << "attack_device_distinct_account_10m="
              << attack_snapshot.device_distinct_account_10m << '\n';
    std::cout << "attack_ip_topk_estimated_count="
              << attack_snapshot.ip_topk_estimated_count << '\n';
    std::cout << "attack_ip_in_topk="
              << (attack_snapshot.ip_in_topk ? "true" : "false") << '\n';
    std::cout << "attack_cms_risk_behavior_count="
              << attack_snapshot.cms_risk_behavior_count << '\n';

    (void)normal_snapshot;

    return 0;
}