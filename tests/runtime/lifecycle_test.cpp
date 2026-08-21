#include "aegisflow/app/handler.hpp"
#include "aegisflow/log/logger.hpp"
#include "aegisflow/storage/blacklist_redis_store.hpp"
#include "aegisflow/storage/redis_connection.hpp"
#include "aegisflow/timer/timer.hpp"

#include "tests/support/test_harness.hpp"

#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using aegisflow::app::Handler;
using aegisflow::app::HandlerConfig;
using aegisflow::app::HandlerState;
using aegisflow::app::HandlerStatus;
using aegisflow::log::LogLevel;
using aegisflow::log::LogSubmitStatus;
using aegisflow::log::Logger;
using aegisflow::log::LoggerConfig;
using aegisflow::log::LoggerState;
using aegisflow::log::LoggerStatus;
using aegisflow::test::require;
using aegisflow::timer::Timer;
using aegisflow::timer::TimerConfig;
using aegisflow::timer::TimerStatus;

constexpr auto kChildTimeout = 20s;

class TempDirectory final {
public:
    TempDirectory() {
        auto pattern =
            (std::filesystem::temp_directory_path() /
             "aegisflow-lifecycle-XXXXXX")
                .string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        require(created != nullptr, "无法创建 lifecycle 临时目录");
        path_ = created;
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class UnusedTcpEndpoint final {
public:
    UnusedTcpEndpoint() {
        socket_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        require(socket_ >= 0, "无法创建依赖失败测试 socket");

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        require(
            ::bind(
                socket_, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == 0,
            "无法占用依赖失败测试端口"
        );

        socklen_t size = sizeof(address);
        require(
            ::getsockname(
                socket_, reinterpret_cast<sockaddr*>(&address), &size) == 0,
            "无法读取依赖失败测试端口"
        );
        port_ = ntohs(address.sin_port);
        require(port_ != 0, "内核未分配依赖失败测试端口");
        // socket 保持 bind 但不 listen。测试期间其他进程无法抢占
        // 端口，Redis connect 会稳定收到连接拒绝，而不是依赖机器上
        // 某个固定空闲端口。
    }

    ~UnusedTcpEndpoint() {
        if (socket_ >= 0) {
            (void)::close(socket_);
        }
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    int socket_ = -1;
    std::uint16_t port_ = 0;
};

[[nodiscard]] std::size_t threadCount() {
    const std::filesystem::path tasks("/proc/self/task");
    require(
        std::filesystem::is_directory(tasks),
        "无法读取当前进程线程目录"
    );
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry :
         std::filesystem::directory_iterator(tasks)) {
        ++count;
    }
    return count;
}

[[nodiscard]] bool threadsConvergeTo(
    const std::size_t expected,
    const std::chrono::milliseconds timeout = 500ms
) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (threadCount() == expected) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return threadCount() == expected;
}

void runIsolated(
    const std::string_view name,
    const std::function<void()>& scenario
) {
    const pid_t child = ::fork();
    require(child >= 0, "fork lifecycle 子进程失败");
    if (child == 0) {
        (void)::alarm(static_cast<unsigned int>(kChildTimeout.count()));
        try {
            scenario();
            ::_exit(0);
        } catch (const std::exception& error) {
            ::dprintf(
                STDERR_FILENO,
                "[child:%.*s] %s\n",
                static_cast<int>(name.size()),
                name.data(),
                error.what()
            );
            ::_exit(1);
        } catch (...) {
            ::dprintf(
                STDERR_FILENO,
                "[child:%.*s] unknown error\n",
                static_cast<int>(name.size()),
                name.data()
            );
            ::_exit(1);
        }
    }

    const auto deadline = std::chrono::steady_clock::now() + kChildTimeout;
    int status = 0;
    while (true) {
        const pid_t waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) {
            break;
        }
        if (waited < 0) {
            require(errno == EINTR, "waitpid lifecycle 子进程失败");
            continue;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            (void)::kill(child, SIGKILL);
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
            throw std::runtime_error(std::string(name) + " 子进程超时");
        }
        std::this_thread::sleep_for(10ms);
    }

    require(
        WIFEXITED(status) && WEXITSTATUS(status) == 0,
        std::string(name) + " 子进程失败"
    );
}

[[nodiscard]] LoggerConfig loggerConfig(
    const std::filesystem::path& directory
) {
    LoggerConfig config;
    config.level = LogLevel::Debug;
    config.file = directory / "runtime.log";
    config.queue_capacity = 256;
    config.flush_interval = 10ms;
    return config;
}

void startLoggerAndTimer(const std::filesystem::path& directory) {
    auto& logger = Logger::instance();
    require(
        logger.init(loggerConfig(directory)) == LoggerStatus::Ok,
        "Logger 必须先完成初始化"
    );
    require(logger.start() == LoggerStatus::Ok, "Logger 必须最先启动");
    require(
        logger.state() == LoggerState::Running,
        "Logger 启动后必须进入 Running"
    );

    auto& timer = Timer::instance();
    TimerConfig timer_config;
    timer_config.command_capacity = 128;
    timer_config.timer_capacity = 128;
    require(timer.init(timer_config) == TimerStatus::Ok, "Timer 必须初始化");
    require(timer.start() == TimerStatus::Ok, "Timer 必须在 Logger 后启动");
    require(
        logger.log(
            LogLevel::Info, "Timer started", __FILE__, __LINE__) ==
            LogSubmitStatus::Accepted,
        "Timer 启动过程必须仍可写 Logger"
    );
}

void stopTimerAndLogger() {
    auto& logger = Logger::instance();
    auto& timer = Timer::instance();

    timer.stop();
    require(timer.join() == TimerStatus::Ok, "Timer 必须先于 Logger join");
    timer.stop();
    require(
        timer.join() == TimerStatus::Ok,
        "Timer 重复 stop/join 必须保持幂等"
    );
    require(
        logger.log(
            LogLevel::Info, "Timer stopped", __FILE__, __LINE__) ==
            LogSubmitStatus::Accepted,
        "Timer 停止过程完成后 Logger 仍必须可用"
    );

    logger.stop();
    require(logger.join() == LoggerStatus::Ok, "Logger 必须最后 join");
    logger.stop();
    require(
        logger.join() == LoggerStatus::Ok,
        "Logger 重复 stop/join 必须保持幂等"
    );
    require(
        logger.state() == LoggerState::Stopped,
        "Logger 排空后必须进入 Stopped"
    );
}

void loggerAndTimerRepeatedShutdown() {
    runIsolated("logger-timer-repeat", [] {
        const std::size_t baseline_threads = threadCount();
        const TempDirectory temporary;
        startLoggerAndTimer(temporary.path());
        stopTimerAndLogger();
        require(
            threadsConvergeTo(baseline_threads),
            "Logger 与 Timer join 后不得遗留项目线程"
        );
    });
}

[[nodiscard]] HandlerConfig dependencyFailureConfig(
    const std::uint16_t unused_redis_port
) {
    HandlerConfig config;
    config.redis.host = "127.0.0.1";
    config.redis.port = unused_redis_port;
    config.redis.connect_timeout = 100ms;
    config.redis.command_timeout = 100ms;
    config.redis.key_prefix = "aegisflow:lifecycle:dependency-failure";
    config.mysql.host = "127.0.0.1";
    config.mysql.user = "not-used";
    config.mysql.database = "not-used";
    config.blacklist_cache.startup_timeout = 500ms;
    config.shutdown_grace_timeout = 1s;
    config.acceptor.bind_address = "127.0.0.1";
    config.acceptor.port = 0;
    return config;
}

void handlerFailureRollsBackInReverseOrder() {
    runIsolated("handler-dependency-failure", [] {
        const std::size_t baseline_threads = threadCount();
        const TempDirectory temporary;
        startLoggerAndTimer(temporary.path());

        auto& handler = Handler::instance();
        UnusedTcpEndpoint unavailable_redis;
        auto config = dependencyFailureConfig(unavailable_redis.port());
        auto invalid = config;
        invalid.maintenance_pool.thread_count = 2;
        require(
            handler.init(invalid) == HandlerStatus::InvalidConfig,
            "非单线程 maintenance pool 必须在依赖访问前被拒绝"
        );
        require(
            handler.state() == HandlerState::Constructed,
            "无效配置不得污染 Handler 状态"
        );
        require(
            handler.init(config) == HandlerStatus::DependencyFailed,
            "不可达 Redis 必须使 Handler 初始化明确失败"
        );
        require(
            handler.state() == HandlerState::Constructed,
            "依赖失败后 Handler 必须仍可由统一回滚路径停止"
        );

        // 与 main 的失败分支一致：即使 init 未成功，也先收口
        // Handler，再停 Timer，最后让 Logger 排空前两者的生命周期日志。
        handler.stop();
        require(handler.join() == HandlerStatus::Ok, "Handler 回滚必须可 join");
        handler.stop();
        require(
            handler.join() == HandlerStatus::Ok,
            "失败后的 Handler 重复 stop/join 不得死锁"
        );
        require(
            handler.state() == HandlerState::Stopped,
            "依赖失败回滚后 Handler 必须进入 Stopped"
        );
        require(
            Timer::instance().start() == TimerStatus::Ok,
            "Handler 回滚时不得提前停止 Timer"
        );
        require(
            Logger::instance().log(
                LogLevel::Info,
                "Handler dependency rollback complete",
                __FILE__,
                __LINE__) == LogSubmitStatus::Accepted,
            "Handler 回滚完成后 Logger 仍必须可用"
        );

        stopTimerAndLogger();
        require(
            threadsConvergeTo(baseline_threads),
            "部分启动失败回滚后不得遗留项目线程"
        );
    });
}

template <typename Integer>
[[nodiscard]] Integer parseInteger(
    const char* text,
    const std::string_view name
) {
    require(text != nullptr && *text != '\0', std::string(name) + " 为空");
    Integer value{};
    const std::string_view input(text);
    const auto [end, error] =
        std::from_chars(input.data(), input.data() + input.size(), value);
    require(
        error == std::errc{} && end == input.data() + input.size(),
        std::string(name) + " 不是合法整数"
    );
    return value;
}

[[nodiscard]] std::optional<HandlerConfig> integrationConfig() {
    const char* allow = std::getenv("AEGISFLOW_TEST_MYSQL_ALLOW_MUTATION");
    const char* redis_host = std::getenv("AEGISFLOW_TEST_REDIS_HOST");
    const char* redis_port = std::getenv("AEGISFLOW_TEST_REDIS_PORT");
    const char* mysql_host = std::getenv("AEGISFLOW_TEST_MYSQL_HOST");
    const char* mysql_port = std::getenv("AEGISFLOW_TEST_MYSQL_PORT");
    const char* mysql_user = std::getenv("AEGISFLOW_TEST_MYSQL_USER");
    const char* mysql_database = std::getenv("AEGISFLOW_TEST_MYSQL_DATABASE");
    if (allow == nullptr ||
        std::string_view(allow) != "dedicated-test-database" ||
        redis_host == nullptr || redis_port == nullptr ||
        mysql_host == nullptr || mysql_port == nullptr ||
        mysql_user == nullptr || mysql_database == nullptr) {
        return std::nullopt;
    }

    HandlerConfig config;
    config.redis.host = redis_host;
    config.redis.port = parseInteger<std::uint16_t>(
        redis_port, "AEGISFLOW_TEST_REDIS_PORT");
    if (const char* value = std::getenv("AEGISFLOW_TEST_REDIS_USERNAME")) {
        config.redis.username = value;
    }
    if (const char* value = std::getenv("AEGISFLOW_TEST_REDIS_PASSWORD")) {
        config.redis.password = value;
    }
    if (const char* value = std::getenv("AEGISFLOW_TEST_REDIS_DATABASE")) {
        config.redis.database = parseInteger<std::uint32_t>(
            value, "AEGISFLOW_TEST_REDIS_DATABASE");
    }
    require(config.redis.database <= 15, "Redis database 必须在 0..15");
    config.redis.connect_timeout = 1s;
    config.redis.command_timeout = 1s;
    config.redis.key_prefix =
        "aegisflow:lifecycle:" + std::to_string(::getpid());

    config.mysql.host = mysql_host;
    config.mysql.port = parseInteger<std::uint16_t>(
        mysql_port, "AEGISFLOW_TEST_MYSQL_PORT");
    config.mysql.user = mysql_user;
    config.mysql.database = mysql_database;
    if (const char* value = std::getenv("AEGISFLOW_TEST_MYSQL_PASSWORD")) {
        config.mysql.password = value;
    }
    config.mysql.connect_timeout_sec = 2;
    config.mysql.read_timeout_sec = 2;
    config.mysql.write_timeout_sec = 2;

    config.worker_pool.thread_count = 2;
    config.worker_pool.queue_capacity = 32;
    config.maintenance_pool.thread_count = 1;
    config.maintenance_pool.queue_capacity = 16;
    config.event_loops.loop_count = 1;
    config.limits.max_connections = 32;
    config.limits.max_connections_per_loop = 32;
    config.limits.connection_queue_capacity = 16;
    config.limits.business_queue_capacity = 16;
    config.limits.completion_queue_capacity = 32;
    config.acceptor.bind_address = "127.0.0.1";
    config.acceptor.port = 0;
    config.blacklist_cache.startup_timeout = 5s;
    config.blacklist_cache.batch_size = 32;
    config.blacklist_cache.reset_timeout = 2s;
    config.blacklist_maintenance.maintenance_interval = 100ms;
    config.blacklist_maintenance.maintenance_timeout = 1s;
    config.blacklist_maintenance.expire_cleanup_interval = 1s;
    config.blacklist_maintenance.batch_size = 32;
    config.blacklist_maintenance.candidate_batch_size = 16;
    config.candidate_queue_capacity = 64;
    config.feature_state_maintenance.cleanup_interval = 100ms;
    config.shutdown_grace_timeout = 5s;
    return config;
}

[[nodiscard]] bool deleteLifecycleRedisKeys(
    const aegisflow::storage::RedisConfig& config
) noexcept {
    try {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        auto connection =
            aegisflow::storage::RedisConnection::connect(config, deadline);
        if (connection == nullptr) {
            return false;
        }
        const auto keys =
            aegisflow::storage::RedisKeySet::fromPrefix(config.key_prefix)
                .all();
        std::vector<std::string> command{"DEL"};
        command.insert(command.end(), keys.begin(), keys.end());
        const auto result = connection->command(command, deadline);
        if (result.status !=
                aegisflow::storage::RedisCommandStatus::Ok ||
            result.value.kind !=
                aegisflow::storage::RedisValueKind::Integer) {
            return false;
        }

        std::vector<std::string> exists{"EXISTS"};
        exists.insert(exists.end(), keys.begin(), keys.end());
        const auto remaining = connection->command(exists, deadline);
        return remaining.status ==
                   aegisflow::storage::RedisCommandStatus::Ok &&
               remaining.value.kind ==
                   aegisflow::storage::RedisValueKind::Integer &&
               remaining.value.integer == 0;
    } catch (...) {
        return false;
    }
}

class RedisPrefixCleanup final {
public:
    explicit RedisPrefixCleanup(aegisflow::storage::RedisConfig config)
        : config_(std::move(config)) {}

    ~RedisPrefixCleanup() { (void)deleteLifecycleRedisKeys(config_); }

private:
    aegisflow::storage::RedisConfig config_;
};

void normalHandlerLifecycle(const HandlerConfig& config) {
    RedisPrefixCleanup cleanup(config.redis);
    require(
        deleteLifecycleRedisKeys(config.redis),
        "正常生命周期测试前无法清理独立 Redis prefix"
    );

    runIsolated("handler-normal-lifecycle", [config] {
        const std::size_t baseline_threads = threadCount();
        const TempDirectory temporary;
        startLoggerAndTimer(temporary.path());

        auto& handler = Handler::instance();
        require(
            handler.init(config) == HandlerStatus::Ok,
            "真实 Redis/MySQL 下 Handler 必须初始化"
        );
        require(handler.start() == HandlerStatus::Ok, "Handler 必须最后启动");
        require(
            handler.state() == HandlerState::Running,
            "Handler 启动后必须进入 Running"
        );

        handler.stop();
        require(
            handler.join() == HandlerStatus::Ok,
            "无业务积压时 Handler 必须先排空并 join"
        );
        handler.stop();
        require(
            handler.join() == HandlerStatus::Ok,
            "正常 Handler 重复 stop/join 必须保持幂等"
        );
        require(
            handler.state() == HandlerState::Stopped,
            "Handler join 后必须进入 Stopped"
        );
        require(
            Timer::instance().start() == TimerStatus::Ok,
            "Handler 停止后 Timer 仍必须运行"
        );
        require(
            Logger::instance().log(
                LogLevel::Info,
                "Handler stopped before Timer",
                __FILE__,
                __LINE__) == LogSubmitStatus::Accepted,
            "Handler 停止过程必须仍可写 Logger"
        );

        stopTimerAndLogger();
        require(
            threadsConvergeTo(baseline_threads),
            "完整运行时 join 后不得遗留项目线程"
        );
    });

    require(
        deleteLifecycleRedisKeys(config.redis),
        "正常生命周期测试后无法清理独立 Redis prefix"
    );
}

void partialHandlerStartRollsBack(HandlerConfig config) {
    config.redis.key_prefix += ":partial-start";
    RedisPrefixCleanup cleanup(config.redis);
    require(
        deleteLifecycleRedisKeys(config.redis),
        "部分启动测试前无法清理独立 Redis prefix"
    );

    runIsolated("handler-partial-start-failure", [config]() mutable {
        const std::size_t baseline_threads = threadCount();
        const TempDirectory temporary;
        startLoggerAndTimer(temporary.path());

        // init 先在真实 Redis/MySQL 上完成冷启。start 随后已经
        // 创建业务池、维护池和 EventLoop，最后 Acceptor 因端口
        // 被占用失败，因此这一用例验证真正的部分启动回滚。
        UnusedTcpEndpoint occupied_endpoint;
        config.acceptor.port = occupied_endpoint.port();

        auto& handler = Handler::instance();
        require(
            handler.init(config) == HandlerStatus::Ok,
            "部分启动测试的 Handler init 必须成功"
        );
        require(
            handler.start() == HandlerStatus::StartFailed,
            "Acceptor 端口冲突必须使 start 明确失败"
        );
        require(
            handler.state() == HandlerState::Failed,
            "部分启动失败后 Handler 必须进入 Failed"
        );
        handler.stop();
        require(
            handler.join() == HandlerStatus::StartFailed,
            "部分启动回滚必须保留 StartFailed 结果"
        );
        handler.stop();
        require(
            handler.join() == HandlerStatus::StartFailed,
            "部分启动失败后重复 stop/join 必须幂等"
        );
        require(
            Timer::instance().start() == TimerStatus::Ok,
            "Handler 部分启动回滚不得停止 Timer"
        );
        require(
            Logger::instance().log(
                LogLevel::Info,
                "Handler partial start rollback complete",
                __FILE__,
                __LINE__) == LogSubmitStatus::Accepted,
            "Handler 部分启动回滚后 Logger 仍必须可用"
        );

        stopTimerAndLogger();
        require(
            threadsConvergeTo(baseline_threads),
            "Handler 部分启动失败回滚后不得遗留项目线程"
        );
    });

    require(
        deleteLifecycleRedisKeys(config.redis),
        "部分启动测试后无法清理独立 Redis prefix"
    );
}

}  // namespace

int main(const int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "--integration") {
        try {
            const auto config = integrationConfig();
            if (!config.has_value()) {
                std::cout
                    << "[SKIP] lifecycle_integration: set dedicated "
                       "AEGISFLOW_TEST_REDIS_HOST/PORT, "
                       "AEGISFLOW_TEST_MYSQL_HOST/PORT/USER/DATABASE and "
                       "AEGISFLOW_TEST_MYSQL_ALLOW_MUTATION="
                       "dedicated-test-database\n";
                return 0;
            }
            return aegisflow::test::runModule(
                "lifecycle_integration",
                {
                    {"真实 Handler 正常启停", [config] {
                         normalHandlerLifecycle(*config);
                     }},
                    {"真实 Handler 部分启动失败回滚", [config] {
                         partialHandlerStartRollsBack(*config);
                     }},
                }
            );
        } catch (const std::exception& error) {
            std::cerr << "lifecycle_integration: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc != 1) {
        std::cerr << "usage: test_lifecycle [--integration]\n";
        return 2;
    }

    return aegisflow::test::runModule(
        "lifecycle",
        {
            {"Logger/Timer 重复逆序停机", loggerAndTimerRepeatedShutdown},
            {"Handler 部分启动失败逆序回滚",
             handlerFailureRollsBackInReverseOrder},
        }
    );
}
