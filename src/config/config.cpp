#include "aegisflow/config/config.hpp"

#include "aegisflow/base/array_view.hpp"
#include "aegisflow/net/protocol_contract.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace aegisflow::config {
namespace {

using namespace std::chrono_literals;

constexpr std::array<std::string_view, 45> kServerConfigKeys = {
    "log.level",
    "log.file",
    "log.queue_capacity",
    "log.flush_interval_ms",
    "server.host",
    "server.port",
    "server.io_threads",
    "server.max_connections",
    "server.max_frame_payload_bytes",
    "server.idle_timeout_ms",
    "server.io_timeout_ms",
    "server.business_timeout_ms",
    "server.shutdown_grace_timeout_ms",
    "worker_pool.threads",
    "worker_pool.queue_capacity",
    "maintenance.interval_ms",
    "maintenance.user_state_ttl_ms",
    "maintenance.ip_state_ttl_ms",
    "maintenance.device_state_ttl_ms",
    "policy.user_failure_review_threshold",
    "policy.ip_spray_review_threshold",
    "policy.device_sharing_review_threshold",
    "policy.ip_distinct_reject_threshold",
    "policy.ip_failure_reject_threshold",
    "mysql.host",
    "mysql.port",
    "mysql.user",
    "mysql.password",
    "mysql.database",
    "blacklist.startup_timeout_ms",
    "blacklist.batch_size",
    "blacklist.reset_timeout_ms",
    "blacklist.maintenance_interval_ms",
    "blacklist.maintenance_timeout_ms",
    "blacklist.expire_cleanup_interval_ms",
    "blacklist.candidate_queue_capacity",
    "blacklist.candidate_batch_size",
    "redis.host",
    "redis.port",
    "redis.username",
    "redis.password",
    "redis.database",
    "redis.connect_timeout_ms",
    "redis.command_timeout_ms",
    "redis.key_prefix",
};

std::string trim(const std::string& value) {
    const auto begin = std::find_if_not(
        value.begin(), value.end(), [](const unsigned char ch) {
            return std::isspace(ch);
        });
    const auto end = std::find_if_not(
        value.rbegin(), value.rend(), [](const unsigned char ch) {
            return std::isspace(ch);
        }).base();
    return begin >= end ? std::string{} : std::string(begin, end);
}

class RawConfig final {
public:
    explicit RawConfig(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) {
            throw std::invalid_argument("无法打开配置文件: " + path);
        }

        std::string line;
        std::size_t line_no = 0;
        while (std::getline(file, line)) {
            ++line_no;
            const std::string normalized = trim(line);
            if (normalized.empty() || normalized.front() == '#') {
                continue;
            }
            const auto separator = normalized.find('=');
            if (separator == std::string::npos) {
                throw std::invalid_argument(
                    "配置第 " + std::to_string(line_no) + " 行缺少 =");
            }
            const std::string key = trim(normalized.substr(0, separator));
            const std::string value = trim(normalized.substr(separator + 1));
            if (key.empty()) {
                throw std::invalid_argument(
                    "配置第 " + std::to_string(line_no) + " 行键为空");
            }
            if (!values_.emplace(key, value).second) {
                throw std::invalid_argument("重复配置键: " + key);
            }
        }
    }

    [[nodiscard]] std::string getString(
        const std::string& key,
        const std::string& default_value
    ) const {
        const auto value = values_.find(key);
        return value == values_.end() ? default_value : value->second;
    }

    [[nodiscard]] std::uint64_t getUInt64(
        const std::string& key,
        const std::uint64_t default_value
    ) const {
        const auto value = values_.find(key);
        if (value == values_.end()) {
            return default_value;
        }
        if (!value->second.empty() && value->second.front() == '-') {
            throw std::invalid_argument("配置 " + key + " 不能为负数");
        }
        try {
            std::size_t parsed = 0;
            const auto number = std::stoull(value->second, &parsed);
            if (parsed != value->second.size()) {
                throw std::invalid_argument("尾随字符");
            }
            return number;
        } catch (const std::exception&) {
            throw std::invalid_argument("配置 " + key + " 不是无符号整数");
        }
    }

    void requireKnownKeys(
        const base::ArrayView<const std::string_view> known_keys
    ) const {
        for (const auto& [key, unused_value] : values_) {
            static_cast<void>(unused_value);
            if (std::find(known_keys.begin(), known_keys.end(), key) ==
                known_keys.end()) {
                throw std::invalid_argument("未知配置键: " + key);
            }
        }
    }

private:
    std::unordered_map<std::string, std::string> values_;
};

std::size_t readSize(
    const RawConfig& config,
    const std::string& key,
    const std::size_t default_value
) {
    const auto value = config.getUInt64(key, default_value);
    if (value > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("配置 " + key + " 超出 size_t 范围");
    }
    return static_cast<std::size_t>(value);
}

std::uint32_t readUint32(
    const RawConfig& config,
    const std::string& key,
    const std::uint32_t default_value
) {
    const auto value = config.getUInt64(key, default_value);
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("配置 " + key + " 超出 uint32 范围");
    }
    return static_cast<std::uint32_t>(value);
}

std::uint16_t readPort(
    const RawConfig& config,
    const std::string& key,
    const std::uint16_t default_value
) {
    const auto value = config.getUInt64(key, default_value);
    if (value == 0 || value > 65535) {
        throw std::invalid_argument("配置 " + key + " 不是有效端口");
    }
    return static_cast<std::uint16_t>(value);
}

std::string readNonEmptyString(
    const RawConfig& config,
    const std::string& key,
    const std::string& default_value
) {
    auto value = config.getString(key, default_value);
    if (value.empty()) {
        throw std::invalid_argument("配置 " + key + " 不能为空");
    }
    return value;
}

std::chrono::milliseconds readPositiveMilliseconds(
    const RawConfig& config,
    const std::string& key,
    const std::chrono::milliseconds default_value
) {
    constexpr auto maximum = std::chrono::duration_cast<
        std::chrono::milliseconds>(std::chrono::hours(24)).count();
    const auto value = config.getUInt64(
        key, static_cast<std::uint64_t>(default_value.count()));
    if (value == 0 || value > static_cast<std::uint64_t>(maximum)) {
        throw std::invalid_argument(
            "配置 " + key + " 必须在 1 毫秒到 24 小时之间");
    }
    return std::chrono::milliseconds(value);
}

std::size_t addChecked(
    const std::size_t left,
    const std::size_t right,
    const std::string_view name
) {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        throw std::invalid_argument(std::string(name) + " 容量溢出");
    }
    return left + right;
}

log::LogLevel readLogLevel(
    const RawConfig& config,
    const std::string& key,
    const log::LogLevel default_value
) {
    const std::string default_text = [default_value] {
        switch (default_value) {
            case log::LogLevel::Debug:
                return std::string("DEBUG");
            case log::LogLevel::Info:
                return std::string("INFO");
            case log::LogLevel::Warn:
                return std::string("WARN");
            case log::LogLevel::Error:
                return std::string("ERROR");
        }
        return std::string("INFO");
    }();
    const auto value = config.getString(key, default_text);
    if (value == "DEBUG") {
        return log::LogLevel::Debug;
    }
    if (value == "INFO") {
        return log::LogLevel::Info;
    }
    if (value == "WARN") {
        return log::LogLevel::Warn;
    }
    if (value == "ERROR") {
        return log::LogLevel::Error;
    }
    throw std::invalid_argument(
        "配置 " + key + " 必须是 DEBUG、INFO、WARN 或 ERROR"
    );
}

}  // 命名空间

AppConfig loadAppConfig(const std::string& path) {
    const RawConfig config(path);
    config.requireKnownKeys(kServerConfigKeys);

    AppConfig app;
    app.logger.level = readLogLevel(
        config, "log.level", app.logger.level);
    app.logger.file = readNonEmptyString(
        config, "log.file", app.logger.file.string());
    app.logger.queue_capacity = readSize(
        config, "log.queue_capacity", app.logger.queue_capacity);
    app.logger.flush_interval = readPositiveMilliseconds(
        config, "log.flush_interval_ms", app.logger.flush_interval);
    if (app.logger.queue_capacity == 0) {
        throw std::invalid_argument(
            "配置 log.queue_capacity 必须大于 0");
    }

    auto& handler = app.handler;
    handler.acceptor.bind_address = readNonEmptyString(
        config, "server.host", handler.acceptor.bind_address);
    in_addr bind_address{};
    if (::inet_pton(
            AF_INET,
            handler.acceptor.bind_address.c_str(),
            &bind_address) != 1) {
        throw std::invalid_argument(
            "配置 server.host 必须是有效的 IPv4 监听地址");
    }
    handler.acceptor.port = readPort(
        config, "server.port", handler.acceptor.port);
    handler.event_loops.loop_count = readSize(
        config, "server.io_threads", handler.event_loops.loop_count);
    if (handler.event_loops.loop_count == 0 ||
        handler.event_loops.loop_count >
            std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(
            "配置 server.io_threads 必须在 1 到 uint32 上限之间");
    }

    handler.limits.max_connections = readSize(
        config, "server.max_connections", handler.limits.max_connections);
    if (handler.limits.max_connections == 0) {
        throw std::invalid_argument("配置 server.max_connections 必须大于 0");
    }
    handler.limits.max_connections_per_loop =
        handler.limits.max_connections / handler.event_loops.loop_count +
        (handler.limits.max_connections % handler.event_loops.loop_count != 0);
    handler.limits.connection_queue_capacity = std::min<std::size_t>(
        1024, handler.limits.max_connections_per_loop);

    handler.worker_pool.thread_count = readSize(
        config, "worker_pool.threads", handler.worker_pool.thread_count);
    if (handler.worker_pool.thread_count == 0) {
        throw std::invalid_argument("配置 worker_pool.threads 必须大于 0");
    }
    handler.limits.business_queue_capacity = readSize(
        config,
        "worker_pool.queue_capacity",
        handler.limits.business_queue_capacity
    );
    if (handler.limits.business_queue_capacity == 0) {
        throw std::invalid_argument(
            "配置 worker_pool.queue_capacity 必须大于 0");
    }
    const auto completion_coverage = addChecked(
        handler.limits.business_queue_capacity,
        handler.worker_pool.thread_count,
        "completion queue"
    );
    handler.limits.completion_queue_capacity = std::max<std::size_t>(
        2048, completion_coverage);

    handler.limits.max_frame_payload_bytes = readSize(
        config,
        "server.max_frame_payload_bytes",
        net::protocol::kMaxPayloadSize
    );
    if (handler.limits.max_frame_payload_bytes == 0 ||
        handler.limits.max_frame_payload_bytes > net::protocol::kMaxPayloadSize) {
        throw std::invalid_argument(
            "配置 server.max_frame_payload_bytes 超出协议范围");
    }
    const auto frame_limit = addChecked(
        net::protocol::kFrameHeaderSize,
        handler.limits.max_frame_payload_bytes,
        "frame"
    );
    const auto soft_watermark = std::min<std::size_t>(64U * 1024U, frame_limit);
    handler.limits.input_soft_watermark_bytes = soft_watermark;
    handler.limits.max_input_buffer_bytes = frame_limit;
    handler.limits.output_soft_watermark_bytes = soft_watermark;
    handler.limits.max_output_buffer_bytes = frame_limit;
    handler.limits.completion_queue_byte_capacity = std::max<std::size_t>(
        16U * 1024U * 1024U, frame_limit);

    const auto idle_timeout = readPositiveMilliseconds(
        config, "server.idle_timeout_ms", 30'000ms);
    const auto io_timeout = readPositiveMilliseconds(
        config, "server.io_timeout_ms", 10'000ms);
    handler.deadlines.enabled = true;
    handler.deadlines.idle_timeout = idle_timeout;
    handler.deadlines.read_timeout = io_timeout;
    handler.deadlines.write_timeout = io_timeout;
    handler.deadlines.business_timeout = readPositiveMilliseconds(
        config, "server.business_timeout_ms", 2'000ms);
    handler.shutdown_grace_timeout = readPositiveMilliseconds(
        config, "server.shutdown_grace_timeout_ms", 5'000ms);

    const auto maintenance_interval = readPositiveMilliseconds(
        config, "maintenance.interval_ms", 60'000ms);
    // 冷状态回收保持独立周期；黑名单维护由下方专用配置驱动。
    handler.feature_state_maintenance.cleanup_interval =
        maintenance_interval;
    handler.feature_reclamation.user_ttl_ms = config.getUInt64(
        "maintenance.user_state_ttl_ms", 10ULL * 60ULL * 1000ULL);
    handler.feature_reclamation.ip_ttl_ms = config.getUInt64(
        "maintenance.ip_state_ttl_ms", 20ULL * 60ULL * 1000ULL);
    handler.feature_reclamation.device_ttl_ms = config.getUInt64(
        "maintenance.device_state_ttl_ms", 20ULL * 60ULL * 1000ULL);
    constexpr std::uint64_t kUserFeatureWindowMs =
        5ULL * 60ULL * 1000ULL;
    if (handler.feature_reclamation.user_ttl_ms < kUserFeatureWindowMs ||
        handler.feature_reclamation.ip_ttl_ms <
            feature::LoginFeatureStore::kDistinctWindowMs ||
        handler.feature_reclamation.device_ttl_ms <
            feature::LoginFeatureStore::kDistinctWindowMs) {
        throw std::invalid_argument(
            "特征状态 TTL 不得短于对应滑动窗口");
    }

    handler.policy.user_failure_review_threshold = readUint32(
        config,
        "policy.user_failure_review_threshold",
        handler.policy.user_failure_review_threshold
    );
    handler.policy.ip_spray_review_threshold = readUint32(
        config,
        "policy.ip_spray_review_threshold",
        handler.policy.ip_spray_review_threshold
    );
    handler.policy.device_sharing_review_threshold = readUint32(
        config,
        "policy.device_sharing_review_threshold",
        handler.policy.device_sharing_review_threshold
    );
    handler.policy.ip_distinct_reject_threshold = readUint32(
        config,
        "policy.ip_distinct_reject_threshold",
        handler.policy.ip_distinct_reject_threshold
    );
    handler.policy.ip_failure_reject_threshold = readUint32(
        config,
        "policy.ip_failure_reject_threshold",
        handler.policy.ip_failure_reject_threshold
    );
    risk::validateLoginPolicyConfig(handler.policy);

    handler.mysql.host = readNonEmptyString(
        config, "mysql.host", handler.mysql.host);
    handler.mysql.port = readPort(config, "mysql.port", handler.mysql.port);
    handler.mysql.user = readNonEmptyString(
        config, "mysql.user", handler.mysql.user);
    handler.mysql.password = config.getString(
        "mysql.password", handler.mysql.password);
    handler.mysql.database = readNonEmptyString(
        config, "mysql.database", handler.mysql.database);

    handler.blacklist_cache.startup_timeout = readPositiveMilliseconds(
        config,
        "blacklist.startup_timeout_ms",
        handler.blacklist_cache.startup_timeout);
    handler.blacklist_cache.batch_size = readSize(
        config, "blacklist.batch_size", handler.blacklist_cache.batch_size);
    handler.blacklist_cache.reset_timeout = readPositiveMilliseconds(
        config,
        "blacklist.reset_timeout_ms",
        handler.blacklist_cache.reset_timeout);
    if (handler.blacklist_cache.batch_size == 0) {
        throw std::invalid_argument("blacklist.batch_size 必须大于 0");
    }
    handler.blacklist_maintenance.maintenance_interval =
        readPositiveMilliseconds(
            config,
            "blacklist.maintenance_interval_ms",
            handler.blacklist_maintenance.maintenance_interval);
    handler.blacklist_maintenance.maintenance_timeout =
        readPositiveMilliseconds(
            config,
            "blacklist.maintenance_timeout_ms",
            handler.blacklist_maintenance.maintenance_timeout);
    handler.blacklist_maintenance.expire_cleanup_interval =
        readPositiveMilliseconds(
            config,
            "blacklist.expire_cleanup_interval_ms",
            handler.blacklist_maintenance.expire_cleanup_interval);
    handler.blacklist_maintenance.batch_size =
        handler.blacklist_cache.batch_size;
    handler.blacklist_maintenance.candidate_batch_size = readSize(
        config,
        "blacklist.candidate_batch_size",
        handler.blacklist_maintenance.candidate_batch_size);
    handler.candidate_queue_capacity = readSize(
        config,
        "blacklist.candidate_queue_capacity",
        handler.candidate_queue_capacity);
    if (handler.candidate_queue_capacity == 0 ||
        handler.blacklist_maintenance.candidate_batch_size == 0 ||
        handler.blacklist_maintenance.candidate_batch_size >
            handler.candidate_queue_capacity) {
        throw std::invalid_argument("黑名单候选队列或批量配置无效");
    }

    handler.redis.host = readNonEmptyString(
        config, "redis.host", handler.redis.host);
    handler.redis.port = readPort(config, "redis.port", handler.redis.port);
    handler.redis.username = config.getString(
        "redis.username", handler.redis.username);
    handler.redis.password = config.getString(
        "redis.password", handler.redis.password);
    handler.redis.database = readUint32(
        config, "redis.database", handler.redis.database);
    handler.redis.connect_timeout = readPositiveMilliseconds(
        config, "redis.connect_timeout_ms", handler.redis.connect_timeout);
    handler.redis.command_timeout = readPositiveMilliseconds(
        config, "redis.command_timeout_ms", handler.redis.command_timeout);
    handler.redis.key_prefix = readNonEmptyString(
        config, "redis.key_prefix", handler.redis.key_prefix);
    if (handler.redis.database > 15 ||
        (!handler.redis.username.empty() && handler.redis.password.empty())) {
        throw std::invalid_argument("Redis 连接或 key prefix 配置无效");
    }

    app.timer.command_capacity = std::max<std::size_t>(
        4096,
        addChecked(
            handler.limits.business_queue_capacity,
            handler.event_loops.loop_count,
            "timer command"
        )
    );
    app.timer.timer_capacity = std::max<std::size_t>(
        1024,
        addChecked(
            handler.limits.max_connections,
            handler.limits.max_connections,
            "timer"
        )
    );
    return app;
}

}  // 命名空间 aegisflow::config
