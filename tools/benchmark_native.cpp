#include "aegisflow/benchmark/load_generator.hpp"

#include <google/protobuf/stubs/common.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>

namespace benchmark = aegisflow::benchmark;

namespace {

std::string trim(std::string value) {
    const auto non_space = [](const unsigned char character) {
        return std::isspace(character) == 0;
    };
    value.erase(
        value.begin(), std::find_if(value.begin(), value.end(), non_space)
    );
    value.erase(
        std::find_if(value.rbegin(), value.rend(), non_space).base(),
        value.end()
    );
    return value;
}

std::uint64_t readUnsigned(
    const std::string_view value,
    const std::string_view name
) {
    if (value.empty() ||
        !std::all_of(value.begin(), value.end(), [](const unsigned char byte) {
            return std::isdigit(byte) != 0;
        })) {
        throw std::invalid_argument(
            std::string(name) + " 不是有效无符号整数"
        );
    }
    std::size_t parsed_chars = 0;
    std::uint64_t parsed = 0;
    try {
        parsed = std::stoull(std::string(value), &parsed_chars, 10);
    } catch (const std::exception&) {
        throw std::invalid_argument(
            std::string(name) + " 不是有效无符号整数"
        );
    }
    if (parsed_chars != value.size()) {
        throw std::invalid_argument(
            std::string(name) + " 含有非整数字符"
        );
    }
    return parsed;
}

std::size_t readSize(
    const std::string_view value,
    const std::string_view name
) {
    const auto parsed = readUnsigned(value, name);
    if (parsed > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument(std::string(name) + " 超出 size_t");
    }
    return static_cast<std::size_t>(parsed);
}

double readDouble(
    const std::string_view value,
    const std::string_view name
) {
    std::size_t parsed_chars = 0;
    double parsed = 0.0;
    try {
        parsed = std::stod(std::string(value), &parsed_chars);
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string(name) + " 不是有效数字");
    }
    if (parsed_chars != value.size()) {
        throw std::invalid_argument(
            std::string(name) + " 含有非数字字符"
        );
    }
    return parsed;
}

std::chrono::milliseconds readMilliseconds(
    const std::string_view value,
    const std::string_view name
) {
    const auto parsed = readUnsigned(value, name);
    if (parsed > static_cast<std::uint64_t>(
                     std::numeric_limits<std::int64_t>::max()
                 )) {
        throw std::invalid_argument(std::string(name) + " 超出时长范围");
    }
    return std::chrono::milliseconds(parsed);
}

void applyValue(
    benchmark::LoadConfig& config,
    const std::string_view key,
    const std::string_view value
) {
    if (key == "host") {
        config.host = value;
    } else if (key == "port") {
        const auto port = readUnsigned(value, key);
        if (port > std::numeric_limits<std::uint16_t>::max()) {
            throw std::invalid_argument("port 超出 uint16 范围");
        }
        config.port = static_cast<std::uint16_t>(port);
    } else if (key == "request_concurrency") {
        config.request_concurrency = readSize(value, key);
    } else if (key == "connection_pool_size") {
        config.connection_pool_size = readSize(value, key);
    } else if (key == "requests_per_connection") {
        config.requests_per_connection = readSize(value, key);
    } else if (key == "target_qps") {
        config.target_qps = readDouble(value, key);
    } else if (key == "warmup_ms") {
        config.warmup = readMilliseconds(value, key);
    } else if (key == "duration_ms") {
        config.duration = readMilliseconds(value, key);
    } else if (key == "connect_timeout_ms") {
        config.connect_timeout = readMilliseconds(value, key);
    } else if (key == "request_timeout_ms") {
        config.request_timeout = readMilliseconds(value, key);
    } else if (key == "seed") {
        config.seed = readUnsigned(value, key);
    } else if (key == "attack_ratio") {
        config.attack_ratio = readDouble(value, key);
    } else if (key == "normal_users") {
        config.normal_users = readSize(value, key);
    } else if (key == "normal_ip_count") {
        config.normal_ip_count = readSize(value, key);
    } else if (key == "normal_device_count") {
        config.normal_device_count = readSize(value, key);
    } else if (key == "attack_users") {
        config.attack_users = readSize(value, key);
    } else if (key == "attack_ip") {
        config.attack_ip = value;
    } else if (key == "attack_device") {
        config.attack_device = value;
    } else if (key == "entity_prefix") {
        config.entity_prefix = value;
    } else {
        throw std::invalid_argument(
            "未知 benchmark 配置项: " + std::string(key)
        );
    }
}

void loadConfigFile(
    benchmark::LoadConfig& config,
    const std::string& path
) {
    std::ifstream input(path);
    if (!input.is_open()) {
        throw std::runtime_error("无法打开 benchmark 配置: " + path);
    }

    std::string line;
    std::size_t line_number = 0;
    std::unordered_set<std::string> seen_keys;
    while (std::getline(input, line)) {
        ++line_number;
        const auto comment = line.find('#');
        if (comment != std::string::npos) {
            line.erase(comment);
        }
        line = trim(std::move(line));
        if (line.empty()) {
            continue;
        }
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            throw std::invalid_argument(
                "benchmark 配置第 " + std::to_string(line_number) +
                " 行缺少 ="
            );
        }
        const auto key = trim(line.substr(0, separator));
        const auto value = trim(line.substr(separator + 1));
        if (key.empty()) {
            throw std::invalid_argument(
                "benchmark 配置第 " + std::to_string(line_number) +
                " 行 key 为空"
            );
        }
        if (!seen_keys.insert(key).second) {
            throw std::invalid_argument("benchmark 配置键重复: " + key);
        }
        applyValue(config, key, value);
    }
}

void printUsage(const char* program) {
    std::cout
        << "用法: " << program << " [选项]\n"
        << "  --config FILE                    先读取独立负载配置\n"
        << "  --host HOST                      服务地址\n"
        << "  --port PORT                      服务端口\n"
        << "  --request-concurrency N          同时在途请求上限\n"
        << "  --connection-pool-size N         连接池大小\n"
        << "  --requests-per-connection N      每次连接的请求上限\n"
        << "  --target-qps N                   全局目标 QPS，0 为不限速\n"
        << "  --warmup-ms N                    预热时间\n"
        << "  --duration-ms N                  计量时间\n"
        << "  --connect-timeout-ms N           连接超时\n"
        << "  --request-timeout-ms N           请求超时\n"
        << "  --seed N                         固定负载种子\n"
        << "  --attack-ratio R                 攻击流量比例 [0,1]\n"
        << "  --attack-ip IP                   本轮攻击来源 IP\n"
        << "  --attack-device TEXT             本轮攻击设备标识\n"
        << "  --entity-prefix TEXT             本轮实体前缀\n"
        << "  --help                            显示帮助\n";
}

bool parseOptions(
    const int argc,
    char* argv[],
    benchmark::LoadConfig& config
) {
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--help") {
            printUsage(argv[0]);
            return false;
        }
    }

    std::string config_path;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) != "--config") {
            continue;
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument("--config 缺少值");
        }
        if (!config_path.empty()) {
            throw std::invalid_argument("--config 只能出现一次");
        }
        config_path = argv[++index];
    }
    if (!config_path.empty()) {
        loadConfigFile(config, config_path);
    }

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--config") {
            ++index;
            continue;
        }
        if (argument.size() < 2 || argument.compare(0, 2, "--") != 0) {
            throw std::invalid_argument(
                "未知位置参数: " + std::string(argument)
            );
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument(
                std::string(argument) + " 缺少值"
            );
        }
        const std::string value(argv[++index]);
        std::string key(argument.substr(2));
        std::replace(key.begin(), key.end(), '-', '_');
        applyValue(config, key, value);
    }

    const auto validation = benchmark::validateLoadConfig(config);
    if (!validation.ok()) {
        throw std::invalid_argument(validation.message);
    }
    return true;
}

}  // namespace

int main(const int argc, char* argv[]) {
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    try {
        benchmark::LoadConfig config;
        if (!parseOptions(argc, argv, config)) {
            google::protobuf::ShutdownProtobufLibrary();
            return 0;
        }

        const auto result = benchmark::runBenchmark(config);
        std::cout << benchmark::renderKeyValueSummary(result) << '\n';
        const bool successful = result.requestAccountingConsistent() &&
                                result.metrics.failed_requests == 0;
        google::protobuf::ShutdownProtobufLibrary();
        return successful ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "benchmark_native 失败: " << error.what() << '\n';
        google::protobuf::ShutdownProtobufLibrary();
        return 1;
    }
}
