#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "decision.pb.h"
#include "event.pb.h"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace v1 = aegisflow::v1;

using tcp = asio::ip::tcp;

namespace {

struct Options {
    std::string host = "127.0.0.1";
    std::string port = "8080";
    std::string target = "/v1/event/report";
    size_t requests = 10000;
    size_t threads = 4;
    double attack_ratio = 0.20;
    size_t normal_users = 100000;
    size_t normal_ip_num = 1000;
    size_t normal_device_num = 5000;
    size_t attack_users = 100;
    std::string attack_ip = "203.0.113.10";
    std::string attack_device = "attack_device";
    uint64_t timeout_ms = 3000;
};

struct PercentileStats {
    size_t count = 0;
    double avg = 0.0;
    uint64_t p50 = 0;
    uint64_t p95 = 0;
    uint64_t p99 = 0;
    uint64_t max = 0;
};

struct WorkerResult {
    size_t sent = 0;
    size_t success = 0;
    size_t failed = 0;
    size_t normal = 0;
    size_t attack = 0;
    size_t http_errors = 0;
    size_t parse_errors = 0;
    size_t network_errors = 0;
    std::vector<uint64_t> latencies_us;
    std::vector<uint64_t> service_cost_us;
    std::map<std::string, size_t> action_counts;
    std::map<std::string, size_t> reason_counts;
    std::vector<std::string> sample_errors;
};

uint64_t nowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()
        ).count()
    );
}

uint64_t elapsedMicros(std::chrono::steady_clock::time_point start) {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(
            steady_clock::now() - start
        ).count()
    );
}

void printUsage(const char* program) {
    std::cout
        << "Usage: " << program << " [options]\n"
        << "Options:\n"
        << "  --host HOST              server host, default 127.0.0.1\n"
        << "  --port PORT              server port, default 8080\n"
        << "  --target PATH            request path, default /v1/event/report\n"
        << "  --requests N             total request count, default 10000\n"
        << "  --threads N              worker threads / keep-alive connections, default 4\n"
        << "  --connections N          alias of --threads\n"
        << "  --attack-ratio R         attack traffic ratio in [0,1], default 0.20\n"
        << "  --normal-users N         normal user cardinality, default 100000\n"
        << "  --normal-ip-num N        normal IP cardinality, default 1000\n"
        << "  --normal-device-num N    normal device cardinality, default 5000\n"
        << "  --attack-users N         attack user pool size, default 100\n"
        << "  --attack-ip IP           attack source IP, default 203.0.113.10\n"
        << "  --attack-device ID       attack device ID, default attack_device\n"
        << "  --timeout-ms N           connect/read/write timeout, default 3000\n"
        << "  --help                   show this help\n";
}

size_t readSize(const char* value, const std::string& name) {
    try {
        const auto parsed = std::stoull(value);
        return static_cast<size_t>(parsed);
    } catch (const std::exception&) {
        throw std::invalid_argument("invalid integer for " + name + ": " + value);
    }
}

uint64_t readUInt64(const char* value, const std::string& name) {
    try {
        return static_cast<uint64_t>(std::stoull(value));
    } catch (const std::exception&) {
        throw std::invalid_argument("invalid integer for " + name + ": " + value);
    }
}

double readDouble(const char* value, const std::string& name) {
    try {
        return std::stod(value);
    } catch (const std::exception&) {
        throw std::invalid_argument("invalid number for " + name + ": " + value);
    }
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

        if (arg == "--host") {
            options.host = requireValue(arg);
        } else if (arg == "--port") {
            options.port = requireValue(arg);
        } else if (arg == "--target") {
            options.target = requireValue(arg);
        } else if (arg == "--requests") {
            options.requests = readSize(requireValue(arg), arg);
        } else if (arg == "--threads" || arg == "--connections") {
            options.threads = readSize(requireValue(arg), arg);
        } else if (arg == "--attack-ratio") {
            options.attack_ratio = readDouble(requireValue(arg), arg);
        } else if (arg == "--normal-users") {
            options.normal_users = readSize(requireValue(arg), arg);
        } else if (arg == "--normal-ip-num") {
            options.normal_ip_num = readSize(requireValue(arg), arg);
        } else if (arg == "--normal-device-num") {
            options.normal_device_num = readSize(requireValue(arg), arg);
        } else if (arg == "--attack-users") {
            options.attack_users = readSize(requireValue(arg), arg);
        } else if (arg == "--attack-ip") {
            options.attack_ip = requireValue(arg);
        } else if (arg == "--attack-device") {
            options.attack_device = requireValue(arg);
        } else if (arg == "--timeout-ms") {
            options.timeout_ms = readUInt64(requireValue(arg), arg);
        } else if (arg == "--help") {
            printUsage(argv[0]);
            return false;
        } else {
            throw std::invalid_argument("unknown option: " + arg);
        }
    }

    if (options.requests == 0) {
        throw std::invalid_argument("requests must be greater than 0");
    }
    if (options.threads == 0) {
        throw std::invalid_argument("threads must be greater than 0");
    }
    if (options.attack_ratio < 0.0 || options.attack_ratio > 1.0) {
        throw std::invalid_argument("attack-ratio must be in [0,1]");
    }
    if (options.normal_users == 0) {
        throw std::invalid_argument("normal-users must be greater than 0");
    }
    if (options.normal_ip_num == 0) {
        throw std::invalid_argument("normal-ip-num must be greater than 0");
    }
    if (options.normal_device_num == 0) {
        throw std::invalid_argument("normal-device-num must be greater than 0");
    }
    if (options.attack_ratio > 0.0 && options.attack_users == 0) {
        throw std::invalid_argument("attack-users must be greater than 0");
    }
    if (options.timeout_ms == 0) {
        throw std::invalid_argument("timeout-ms must be greater than 0");
    }

    return true;
}

bool isAttackRequest(size_t index, double attack_ratio) {
    if (attack_ratio <= 0.0) {
        return false;
    }
    if (attack_ratio >= 1.0) {
        return true;
    }

    const auto threshold = static_cast<size_t>(attack_ratio * 10000.0 + 0.5);
    return ((index * 7919 + 104729) % 10000) < threshold;
}

std::string makeNormalIp(size_t ip_id) {
    return "198.51." +
        std::to_string((ip_id / 256) % 256) +
        "." +
        std::to_string(ip_id % 256);
}

std::string serializeEventRequest(
    const Options& options,
    size_t index,
    bool attack
) {
    v1::ReportEventRequest request;
    auto* event = request.mutable_event();

    event->set_event_id(static_cast<uint64_t>(index + 1));
    event->set_timestamp_ms(nowMs());
    event->set_scene("login");
    event->set_type(v1::LOGIN);

    if (attack) {
        event->set_user_id(
            "attack_user_" +
            std::to_string(index % options.attack_users)
        );
        event->set_ip(options.attack_ip);
        event->set_device_id(options.attack_device);
        event->set_result(v1::FAIL);
    } else {
        event->set_user_id(
            "normal_user_" +
            std::to_string(index % options.normal_users)
        );
        event->set_ip(makeNormalIp(index % options.normal_ip_num));
        event->set_device_id(
            "normal_device_" +
            std::to_string(index % options.normal_device_num)
        );
        event->set_result(v1::SUCCESS);
    }

    std::string body;
    if (!request.SerializeToString(&body)) {
        throw std::runtime_error("failed to serialize ReportEventRequest");
    }

    return body;
}

std::unique_ptr<beast::tcp_stream> connectStream(
    asio::io_context& io_context,
    tcp::resolver& resolver,
    const Options& options
) {
    auto stream = std::make_unique<beast::tcp_stream>(io_context);
    stream->expires_after(std::chrono::milliseconds(options.timeout_ms));

    auto endpoints = resolver.resolve(options.host, options.port);

    stream->expires_after(std::chrono::milliseconds(options.timeout_ms));
    stream->connect(endpoints);

    return stream;
}

void closeStream(std::unique_ptr<beast::tcp_stream>& stream) {
    if (!stream) {
        return;
    }

    beast::error_code ec;
    stream->socket().shutdown(tcp::socket::shutdown_both, ec);
    stream->socket().close(ec);
    stream.reset();
}

void addSampleError(WorkerResult& result, const std::string& message) {
    if (result.sample_errors.size() < 5) {
        result.sample_errors.push_back(message);
    }
}

std::string actionName(v1::DecisionAction action) {
    switch (action) {
    case v1::PASS:
        return "PASS";
    case v1::REVIEW:
        return "REVIEW";
    case v1::REJECT:
        return "REJECT";
    default:
        return "UNKNOWN";
    }
}

WorkerResult runWorker(
    const Options& options,
    size_t begin_index,
    size_t count
) {
    WorkerResult result;
    result.latencies_us.reserve(count);
    result.service_cost_us.reserve(count);

    asio::io_context io_context;
    tcp::resolver resolver(io_context);
    std::unique_ptr<beast::tcp_stream> stream;
    beast::flat_buffer buffer;

    for (size_t offset = 0; offset < count; ++offset) {
        const size_t index = begin_index + offset;
        const bool attack = isAttackRequest(index, options.attack_ratio);
        const auto start = std::chrono::steady_clock::now();

        ++result.sent;
        if (attack) {
            ++result.attack;
        } else {
            ++result.normal;
        }

        try {
            if (!stream) {
                buffer.consume(buffer.size());
                stream = connectStream(io_context, resolver, options);
            }

            http::request<http::string_body> request{
                http::verb::post,
                options.target,
                11
            };

            request.set(http::field::host, options.host + ":" + options.port);
            request.set(http::field::user_agent, "aegisflow-benchmark-http");
            request.set(http::field::content_type, "application/x-protobuf");
            request.set(http::field::accept, "application/x-protobuf");
            request.keep_alive(true);
            request.body() = serializeEventRequest(options, index, attack);
            request.prepare_payload();

            stream->expires_after(std::chrono::milliseconds(options.timeout_ms));
            http::write(*stream, request);

            http::response<http::string_body> response;
            stream->expires_after(std::chrono::milliseconds(options.timeout_ms));
            http::read(*stream, buffer, response);

            result.latencies_us.push_back(elapsedMicros(start));

            if (response.result() != http::status::ok) {
                ++result.failed;
                ++result.http_errors;
                addSampleError(
                    result,
                    "http " + std::to_string(response.result_int()) +
                    " " + std::string(response.reason())
                );
                if (!response.keep_alive()) {
                    closeStream(stream);
                }
                continue;
            }

            v1::ReportEventResponse response_pb;
            if (!response_pb.ParseFromString(response.body())) {
                ++result.failed;
                ++result.parse_errors;
                addSampleError(result, "failed to parse ReportEventResponse");
                if (!response.keep_alive()) {
                    closeStream(stream);
                }
                continue;
            }

            ++result.success;
            const auto& decision = response_pb.decision();
            ++result.action_counts[actionName(decision.action())];
            result.service_cost_us.push_back(decision.cost_us());

            for (const auto& reason : decision.reasons()) {
                ++result.reason_counts[reason.code()];
            }

            if (!response.keep_alive()) {
                closeStream(stream);
            }
        } catch (const std::exception& ex) {
            result.latencies_us.push_back(elapsedMicros(start));
            ++result.failed;
            ++result.network_errors;
            addSampleError(result, ex.what());
            closeStream(stream);
            buffer.consume(buffer.size());
        }
    }

    closeStream(stream);

    return result;
}

void mergeMap(
    std::map<std::string, size_t>& target,
    const std::map<std::string, size_t>& source
) {
    for (const auto& [key, value] : source) {
        target[key] += value;
    }
}

void mergeResult(WorkerResult& target, WorkerResult&& source) {
    target.sent += source.sent;
    target.success += source.success;
    target.failed += source.failed;
    target.normal += source.normal;
    target.attack += source.attack;
    target.http_errors += source.http_errors;
    target.parse_errors += source.parse_errors;
    target.network_errors += source.network_errors;

    target.latencies_us.insert(
        target.latencies_us.end(),
        source.latencies_us.begin(),
        source.latencies_us.end()
    );
    target.service_cost_us.insert(
        target.service_cost_us.end(),
        source.service_cost_us.begin(),
        source.service_cost_us.end()
    );

    mergeMap(target.action_counts, source.action_counts);
    mergeMap(target.reason_counts, source.reason_counts);

    for (const auto& error : source.sample_errors) {
        addSampleError(target, error);
    }
}

PercentileStats buildStats(std::vector<uint64_t> samples) {
    PercentileStats stats;
    stats.count = samples.size();

    if (samples.empty()) {
        return stats;
    }

    std::sort(samples.begin(), samples.end());

    const long double sum = std::accumulate(
        samples.begin(),
        samples.end(),
        static_cast<long double>(0)
    );

    auto percentile = [&](double p) {
        const auto index = static_cast<size_t>(
            std::min<double>(
                static_cast<double>(samples.size() - 1),
                p * static_cast<double>(samples.size() - 1)
            )
        );
        return samples[index];
    };

    stats.avg = static_cast<double>(sum / samples.size());
    stats.p50 = percentile(0.50);
    stats.p95 = percentile(0.95);
    stats.p99 = percentile(0.99);
    stats.max = samples.back();

    return stats;
}

size_t countForAction(
    const std::map<std::string, size_t>& action_counts,
    const std::string& action
) {
    const auto it = action_counts.find(action);
    if (it == action_counts.end()) {
        return 0;
    }
    return it->second;
}

std::vector<std::pair<std::string, size_t>> sortedCounts(
    const std::map<std::string, size_t>& counts
) {
    std::vector<std::pair<std::string, size_t>> items(
        counts.begin(),
        counts.end()
    );

    std::sort(
        items.begin(),
        items.end(),
        [](const auto& lhs, const auto& rhs) {
            if (lhs.second != rhs.second) {
                return lhs.second > rhs.second;
            }
            return lhs.first < rhs.first;
        }
    );

    return items;
}

} // namespace

int main(int argc, char** argv) {
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    Options options;

    try {
        if (!parseOptions(argc, argv, options)) {
            google::protobuf::ShutdownProtobufLibrary();
            return 0;
        }
    } catch (const std::exception& ex) {
        std::cerr << "invalid options: " << ex.what() << std::endl;
        printUsage(argv[0]);
        google::protobuf::ShutdownProtobufLibrary();
        return 1;
    }

    std::vector<std::thread> threads;
    std::vector<WorkerResult> worker_results(options.threads);
    threads.reserve(options.threads);

    const auto bench_start = std::chrono::steady_clock::now();
    size_t begin = 0;

    for (size_t i = 0; i < options.threads; ++i) {
        const size_t remaining = options.requests - begin;
        const size_t workers_left = options.threads - i;
        const size_t count = (remaining + workers_left - 1) / workers_left;
        const size_t worker_begin = begin;
        begin += count;

        threads.emplace_back(
            [&, i, worker_begin, count]() {
                worker_results[i] = runWorker(options, worker_begin, count);
            }
        );
    }

    for (auto& thread : threads) {
        thread.join();
    }

    const uint64_t total_us = elapsedMicros(bench_start);

    WorkerResult total;
    total.latencies_us.reserve(options.requests);
    total.service_cost_us.reserve(options.requests);

    for (auto& result : worker_results) {
        mergeResult(total, std::move(result));
    }

    const auto latency_stats = buildStats(total.latencies_us);
    const auto service_stats = buildStats(total.service_cost_us);

    const double qps = total_us == 0
        ? 0.0
        : static_cast<double>(total.sent) * 1000000.0 / total_us;
    const double success_qps = total_us == 0
        ? 0.0
        : static_cast<double>(total.success) * 1000000.0 / total_us;
    const double error_rate = total.sent == 0
        ? 0.0
        : static_cast<double>(total.failed) / static_cast<double>(total.sent);

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "benchmark=aegisflow_http\n";
    std::cout << "host=" << options.host << '\n';
    std::cout << "port=" << options.port << '\n';
    std::cout << "target=" << options.target << '\n';
    std::cout << "threads=" << options.threads << '\n';
    std::cout << "requests=" << options.requests << '\n';
    std::cout << "attack_ratio=" << options.attack_ratio << '\n';
    std::cout << "sent=" << total.sent << '\n';
    std::cout << "success=" << total.success << '\n';
    std::cout << "failed=" << total.failed << '\n';
    std::cout << "normal_requests=" << total.normal << '\n';
    std::cout << "attack_requests=" << total.attack << '\n';
    std::cout << "http_errors=" << total.http_errors << '\n';
    std::cout << "parse_errors=" << total.parse_errors << '\n';
    std::cout << "network_errors=" << total.network_errors << '\n';
    std::cout << "duration_us=" << total_us << '\n';
    std::cout << "qps=" << qps << '\n';
    std::cout << "success_qps=" << success_qps << '\n';
    std::cout << "error_rate=" << error_rate << '\n';
    std::cout << "latency_count=" << latency_stats.count << '\n';
    std::cout << "latency_avg_us=" << latency_stats.avg << '\n';
    std::cout << "latency_p50_us=" << latency_stats.p50 << '\n';
    std::cout << "latency_p95_us=" << latency_stats.p95 << '\n';
    std::cout << "latency_p99_us=" << latency_stats.p99 << '\n';
    std::cout << "latency_max_us=" << latency_stats.max << '\n';
    std::cout << "service_cost_count=" << service_stats.count << '\n';
    std::cout << "service_cost_avg_us=" << service_stats.avg << '\n';
    std::cout << "service_cost_p50_us=" << service_stats.p50 << '\n';
    std::cout << "service_cost_p95_us=" << service_stats.p95 << '\n';
    std::cout << "service_cost_p99_us=" << service_stats.p99 << '\n';
    std::cout << "service_cost_max_us=" << service_stats.max << '\n';
    std::cout << "action_PASS=" << countForAction(total.action_counts, "PASS") << '\n';
    std::cout << "action_REVIEW=" << countForAction(total.action_counts, "REVIEW") << '\n';
    std::cout << "action_REJECT=" << countForAction(total.action_counts, "REJECT") << '\n';
    std::cout << "action_UNKNOWN=" << countForAction(total.action_counts, "UNKNOWN") << '\n';

    for (const auto& [reason, count] : sortedCounts(total.reason_counts)) {
        std::cout << "reason_" << reason << "=" << count << '\n';
    }

    for (size_t i = 0; i < total.sample_errors.size(); ++i) {
        std::cout << "sample_error_" << (i + 1) << "="
                  << total.sample_errors[i] << '\n';
    }

    google::protobuf::ShutdownProtobufLibrary();

    return total.failed == 0 ? 0 : 2;
}
