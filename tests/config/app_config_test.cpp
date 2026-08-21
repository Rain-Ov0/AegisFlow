#include "aegisflow/config/config.hpp"

#include "tests/support/test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using aegisflow::test::require;

class ConfigFixture final {
public:
    explicit ConfigFixture(const std::string_view contents) {
        static std::atomic<std::uint64_t> sequence{0};
        path_ = std::filesystem::temp_directory_path() /
                ("aegisflow-config-" +
                 std::to_string(sequence.fetch_add(1)) + ".conf");
        std::ofstream output(path_, std::ios::trunc);
        require(output.is_open(), "无法创建配置测试文件");
        output << contents;
        output.close();
    }

    ~ConfigFixture() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    [[nodiscard]] std::string path() const {
        return path_.string();
    }

private:
    std::filesystem::path path_;
};

void requireRejected(
    const std::string_view contents,
    const std::string_view message
) {
    const ConfigFixture fixture(contents);
    bool rejected = false;
    try {
        static_cast<void>(aegisflow::config::loadAppConfig(fixture.path()));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, message);
}

void valuesLandInStrongTypes() {
    const ConfigFixture fixture(
        "log.level = ERROR\n"
        "log.file = /tmp/aegisflow-config-test.log\n"
        "log.queue_capacity = 33\n"
        "log.flush_interval_ms = 17\n"
        "server.host = 127.0.0.7\n"
        "server.port = 18080\n"
        "server.io_threads = 3\n"
        "server.max_connections = 10\n"
        "server.max_frame_payload_bytes = 4096\n"
        "server.idle_timeout_ms = 101\n"
        "server.io_timeout_ms = 102\n"
        "server.business_timeout_ms = 103\n"
        "server.shutdown_grace_timeout_ms = 104\n"
        "worker_pool.threads = 2\n"
        "worker_pool.queue_capacity = 7\n"
        "maintenance.interval_ms = 500\n"
        "maintenance.user_state_ttl_ms = 300001\n"
        "maintenance.ip_state_ttl_ms = 600001\n"
        "maintenance.device_state_ttl_ms = 600002\n"
        "policy.user_failure_review_threshold = 6\n"
        "policy.ip_spray_review_threshold = 21\n"
        "policy.device_sharing_review_threshold = 11\n"
        "policy.ip_distinct_reject_threshold = 51\n"
        "policy.ip_failure_reject_threshold = 501\n"
        "mysql.host = mysql.test\n"
        "mysql.port = 3307\n"
        "mysql.user = learner\n"
        "blacklist.startup_timeout_ms = 701\n"
        "blacklist.batch_size = 37\n"
        "blacklist.reset_timeout_ms = 702\n"
        "blacklist.maintenance_interval_ms = 705\n"
        "blacklist.maintenance_timeout_ms = 706\n"
        "blacklist.expire_cleanup_interval_ms = 707\n"
        "blacklist.candidate_queue_capacity = 41\n"
        "blacklist.candidate_batch_size = 13\n"
        "redis.host = redis.test\n"
        "redis.port = 6381\n"
        "redis.username = learner\n"
        "redis.password = secret\n"
        "redis.database = 15\n"
        "redis.connect_timeout_ms = 703\n"
        "redis.command_timeout_ms = 704\n"
        "redis.key_prefix = test:blacklist\n"
        "mysql.password =\n"
        "mysql.database = learning\n"
    );
    const auto config = aegisflow::config::loadAppConfig(fixture.path());

    require(
        config.logger.level == aegisflow::log::LogLevel::Error &&
            config.logger.file == "/tmp/aegisflow-config-test.log" &&
            config.logger.queue_capacity == 33 &&
            config.logger.flush_interval.count() == 17,
        "Logger 四个配置键必须落入强类型配置"
    );
    require(
        config.handler.acceptor.bind_address == "127.0.0.7" &&
            config.handler.acceptor.port == 18080,
        "监听地址与端口必须落入强类型配置"
    );
    require(config.handler.event_loops.loop_count == 3, "IO 线程数必须落入强类型配置");
    require(
        config.handler.limits.max_connections_per_loop == 4,
        "单 loop 连接容量必须向上取整"
    );
    require(
        config.handler.limits.max_frame_payload_bytes == 4096 &&
            config.handler.limits.max_input_buffer_bytes == 4100,
        "帧上限与缓冲区容量必须共享唯一语义"
    );
    require(
        config.handler.worker_pool.thread_count == 2 &&
            config.handler.limits.business_queue_capacity == 7,
        "Worker 线程与队列容量必须落入强类型配置"
    );
    require(
        config.handler.deadlines.idle_timeout.count() == 101 &&
            config.handler.deadlines.read_timeout.count() == 102 &&
            config.handler.deadlines.write_timeout.count() == 102 &&
            config.handler.deadlines.business_timeout.count() == 103 &&
            config.handler.shutdown_grace_timeout.count() == 104,
        "Session 与停机超时必须落入强类型配置"
    );
    require(config.handler.maintenance_pool.thread_count == 1,
            "运行期黑名单维护必须固定为单 worker 顺序消费");
    require(
        config.handler.feature_state_maintenance.cleanup_interval.count() ==
                500 &&
            config.handler.feature_reclamation.user_ttl_ms == 300001 &&
            config.handler.feature_reclamation.ip_ttl_ms == 600001 &&
            config.handler.feature_reclamation.device_ttl_ms == 600002,
        "特征回收周期与三类 TTL 必须落入强类型配置"
    );
    require(
        config.handler.policy.user_failure_review_threshold == 6 &&
            config.handler.policy.ip_spray_review_threshold == 21 &&
            config.handler.policy.device_sharing_review_threshold == 11 &&
            config.handler.policy.ip_distinct_reject_threshold == 51 &&
            config.handler.policy.ip_failure_reject_threshold == 501,
        "五个策略阈值必须落入强类型配置"
    );
    require(
        config.handler.mysql.host == "mysql.test" &&
            config.handler.mysql.port == 3307 &&
            config.handler.mysql.user == "learner" &&
            config.handler.mysql.password.empty() &&
            config.handler.mysql.database == "learning",
        "MySQL 连接配置必须落入强类型且允许空密码"
    );
    require(
        config.handler.blacklist_cache.startup_timeout.count() == 701 &&
            config.handler.blacklist_cache.batch_size == 37 &&
            config.handler.blacklist_cache.reset_timeout.count() == 702,
        "黑名单冷启、批大小和 reset timeout 必须落入强类型"
    );
    require(
        config.handler.blacklist_maintenance.maintenance_interval.count() ==
                705 &&
            config.handler.blacklist_maintenance.maintenance_timeout.count() ==
                706 &&
            config.handler.blacklist_maintenance.expire_cleanup_interval
                    .count() == 707 &&
            config.handler.blacklist_maintenance.batch_size == 37 &&
            config.handler.candidate_queue_capacity == 41 &&
            config.handler.blacklist_maintenance.candidate_batch_size == 13,
        "黑名单维护周期、超时、清理周期与候选容量必须落入强类型"
    );
    require(
        config.handler.redis.host == "redis.test" &&
            config.handler.redis.port == 6381 &&
            config.handler.redis.username == "learner" &&
            config.handler.redis.password == "secret" &&
            config.handler.redis.database == 15 &&
            config.handler.redis.connect_timeout.count() == 703 &&
            config.handler.redis.command_timeout.count() == 704 &&
            config.handler.redis.key_prefix == "test:blacklist",
        "Redis 八个配置键必须落入强类型"
    );
}

void duplicateAndUnknownKeysAreRejected() {
    requireRejected(
        "server.port = 8080\nserver.port = 8081\n",
        "重复配置键必须被拒绝"
    );
    requireRejected(
        "server.legacy_key = 1\n",
        "未知配置键必须被拒绝"
    );
}

void emptyRequiredFieldsAreRejected() {
    requireRejected("log.file =\n", "空日志路径必须被拒绝");
    requireRejected("server.host =\n", "空监听地址必须被拒绝");
    requireRejected(
        "server.host = localhost\n", "非 IPv4 监听地址必须被拒绝");
    requireRejected("mysql.host =\n", "空 MySQL host 必须被拒绝");
    requireRejected("mysql.user =\n", "空 MySQL user 必须被拒绝");
    requireRejected(
        "mysql.database =\n", "空 MySQL database 必须被拒绝");
    requireRejected("redis.host =\n", "空 Redis host 必须被拒绝");
    requireRejected(
        "redis.key_prefix =\n", "空 Redis key prefix 必须被拒绝");
    requireRejected(
        "redis.username = learner\nredis.password =\n",
        "只有 Redis username 没有 password 必须被拒绝");
}

void capacitiesAndPortsAreValidated() {
    requireRejected("server.port = 0\n", "零服务端口必须被拒绝");
    requireRejected("server.port = 70000\n", "越界服务端口必须被拒绝");
    requireRejected("mysql.port = 0\n", "零 MySQL 端口必须被拒绝");
    requireRejected("redis.port = 65536\n", "越界 Redis 端口必须被拒绝");
    requireRejected(
        "log.queue_capacity = 0\n", "零日志队列容量必须被拒绝");
    requireRejected("server.io_threads = 0\n", "零 IO 线程必须被拒绝");
    requireRejected(
        "server.io_threads = 4294967296\n",
        "IO 线程数超过 loop id 范围必须被拒绝");
    requireRejected(
        "server.max_connections = 0\n", "零最大连接数必须被拒绝");
    requireRejected(
        "server.max_frame_payload_bytes = 0\n",
        "零帧载荷上限必须被拒绝");
    requireRejected("worker_pool.threads = 0\n", "零 worker 线程必须被拒绝");
    requireRejected(
        "worker_pool.queue_capacity = 0\n", "零业务队列容量必须被拒绝");
    requireRejected(
        "blacklist.batch_size = 0\n", "零黑名单批大小必须被拒绝");
    requireRejected(
        "blacklist.candidate_queue_capacity = 0\n",
        "零候选队列容量必须被拒绝");
    requireRejected(
        "blacklist.candidate_batch_size = 0\n",
        "零候选批量必须被拒绝");
    requireRejected(
        "blacklist.candidate_queue_capacity = 2\n"
        "blacklist.candidate_batch_size = 3\n",
        "候选批量不得超过队列容量");
    requireRejected("redis.database = 16\n", "Redis database 越界必须被拒绝");
}

void timeValuesAreValidated() {
    constexpr std::string_view too_long = "86400001";
    requireRejected(
        "log.flush_interval_ms = 0\n", "零日志 flush 周期必须被拒绝");
    requireRejected(
        std::string("log.flush_interval_ms = ") + std::string(too_long) +
            "\n",
        "超过 24 小时的日志 flush 周期必须被拒绝");
    requireRejected("server.idle_timeout_ms = 0\n", "零 idle timeout 必须被拒绝");
    requireRejected("server.io_timeout_ms = 0\n", "零 IO timeout 必须被拒绝");
    requireRejected(
        "server.business_timeout_ms = 0\n", "零业务 timeout 必须被拒绝");
    requireRejected(
        "server.shutdown_grace_timeout_ms = 0\n", "零停机 timeout 必须被拒绝");
    requireRejected(
        "maintenance.interval_ms = 0\n", "零特征回收周期必须被拒绝");
    requireRejected(
        "maintenance.user_state_ttl_ms = 299999\n",
        "user TTL 短于用户窗口必须被拒绝");
    requireRejected(
        "maintenance.ip_state_ttl_ms = 599999\n",
        "IP TTL 短于 distinct 窗口必须被拒绝");
    requireRejected(
        "maintenance.device_state_ttl_ms = 599999\n",
        "device TTL 短于 distinct 窗口必须被拒绝");
    requireRejected(
        "blacklist.startup_timeout_ms = 0\n", "零冷启 timeout 必须被拒绝");
    requireRejected(
        "blacklist.reset_timeout_ms = 0\n", "零 reset timeout 必须被拒绝");
    requireRejected(
        "blacklist.maintenance_interval_ms = 0\n",
        "零黑名单 maintenance 周期必须被拒绝");
    requireRejected(
        "blacklist.maintenance_timeout_ms = 0\n",
        "零黑名单 maintenance timeout 必须被拒绝");
    requireRejected(
        "blacklist.expire_cleanup_interval_ms = 0\n",
        "零过期清理周期必须被拒绝");
    requireRejected(
        "redis.connect_timeout_ms = 0\n", "零 Redis 连接 timeout 必须被拒绝");
    requireRejected(
        "redis.command_timeout_ms = 0\n", "零 Redis 命令 timeout 必须被拒绝");
}

void invalidScalarAndCrossFieldValuesAreRejected() {
    requireRejected("log.level = TRACE\n", "未知日志级别必须被拒绝");
    requireRejected(
        "policy.user_failure_review_threshold = 0\n",
        "零策略阈值必须被拒绝");
    requireRejected(
        "policy.ip_spray_review_threshold = 20\n"
        "policy.ip_distinct_reject_threshold = 19\n",
        "IP REJECT 阈值低于 REVIEW 阈值必须被拒绝"
    );
}

void defaultCredentialsContainNoPlaintextPassword() {
    const ConfigFixture fixture("");
    const auto config = aegisflow::config::loadAppConfig(fixture.path());
    require(
        config.handler.mysql.password.empty() &&
            config.handler.redis.password.empty(),
        "默认 MySQL 与 Redis 配置不得携带明文密码"
    );
}

template <typename Mutator>
void requireAppConfigChangeIsUnequal(
    const aegisflow::config::AppConfig& baseline,
    Mutator mutate,
    const std::string_view message
) {
    auto changed = baseline;
    mutate(changed);
    require(!(changed == baseline), message);
}

void appConfigEqualityCoversEveryMajorCategory() {
    const aegisflow::config::AppConfig baseline;
    const auto equal_copy = baseline;
    require(equal_copy == baseline, "AppConfig 完整副本必须相等");

    requireAppConfigChangeIsUnequal(
        baseline,
        [](auto& config) { ++config.logger.queue_capacity; },
        "Logger 字段变化必须使 AppConfig 不等"
    );
    requireAppConfigChangeIsUnequal(
        baseline,
        [](auto& config) { ++config.timer.command_capacity; },
        "Timer 字段变化必须使 AppConfig 不等"
    );
    requireAppConfigChangeIsUnequal(
        baseline,
        [](auto& config) {
            ++config.handler.policy.user_failure_review_threshold;
        },
        "Policy 字段变化必须使 AppConfig 不等"
    );
    requireAppConfigChangeIsUnequal(
        baseline,
        [](auto& config) { ++config.handler.mysql.port; },
        "MySQL 字段变化必须使 AppConfig 不等"
    );
    requireAppConfigChangeIsUnequal(
        baseline,
        [](auto& config) { ++config.handler.redis.port; },
        "Redis 字段变化必须使 AppConfig 不等"
    );
    requireAppConfigChangeIsUnequal(
        baseline,
        [](auto& config) { ++config.handler.blacklist_cache.batch_size; },
        "Blacklist bootstrap 字段变化必须使 AppConfig 不等"
    );
    requireAppConfigChangeIsUnequal(
        baseline,
        [](auto& config) { ++config.handler.worker_pool.thread_count; },
        "业务 WorkerPool 字段变化必须使 AppConfig 不等"
    );
    requireAppConfigChangeIsUnequal(
        baseline,
        [](auto& config) {
            ++config.handler.maintenance_pool.queue_capacity;
        },
        "维护 WorkerPool 字段变化必须使 AppConfig 不等"
    );
    requireAppConfigChangeIsUnequal(
        baseline,
        [](auto& config) {
            ++config.handler.feature_reclamation.user_ttl_ms;
        },
        "Feature reclamation 字段变化必须使 AppConfig 不等"
    );
    requireAppConfigChangeIsUnequal(
        baseline,
        [](auto& config) {
            config.handler.feature_state_maintenance.cleanup_interval +=
                std::chrono::milliseconds(1);
        },
        "Feature maintenance 字段变化必须使 AppConfig 不等"
    );
    requireAppConfigChangeIsUnequal(
        baseline,
        [](auto& config) {
            ++config.handler.blacklist_maintenance.candidate_batch_size;
        },
        "Blacklist maintenance 字段变化必须使 AppConfig 不等"
    );
    requireAppConfigChangeIsUnequal(
        baseline,
        [](auto& config) { ++config.handler.candidate_queue_capacity; },
        "Candidate queue 字段变化必须使 AppConfig 不等"
    );
    requireAppConfigChangeIsUnequal(
        baseline,
        [](auto& config) { ++config.handler.limits.max_connections; },
        "Limits 字段变化必须使 AppConfig 不等"
    );
    requireAppConfigChangeIsUnequal(
        baseline,
        [](auto& config) { ++config.handler.event_loops.loop_count; },
        "EventLoopGroup 字段变化必须使 AppConfig 不等"
    );
    requireAppConfigChangeIsUnequal(
        baseline,
        [](auto& config) { ++config.handler.acceptor.port; },
        "Acceptor 字段变化必须使 AppConfig 不等"
    );
    requireAppConfigChangeIsUnequal(
        baseline,
        [](auto& config) {
            config.handler.deadlines.enabled =
                !config.handler.deadlines.enabled;
        },
        "Session deadline 字段变化必须使 AppConfig 不等"
    );
    requireAppConfigChangeIsUnequal(
        baseline,
        [](auto& config) {
            config.handler.shutdown_grace_timeout +=
                std::chrono::milliseconds(1);
        },
        "停机超时变化必须使 AppConfig 不等"
    );
}

}  // namespace

int main() {
    return aegisflow::test::runModule(
        "app_config",
        {
            {"强类型映射", valuesLandInStrongTypes},
            {"重复与未知键", duplicateAndUnknownKeysAreRejected},
            {"必填字段", emptyRequiredFieldsAreRejected},
            {"容量与端口", capacitiesAndPortsAreValidated},
            {"时间参数", timeValuesAreValidated},
            {"非法配置值", invalidScalarAndCrossFieldValuesAreRejected},
            {"默认无明文密码", defaultCredentialsContainNoPlaintextPassword},
            {"AppConfig 比较完整性", appConfigEqualityCoversEveryMajorCategory},
        }
    );
}
