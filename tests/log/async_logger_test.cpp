#include "aegisflow/log/logger.hpp"

#include "tests/support/test_harness.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using aegisflow::log::LogLevel;
using aegisflow::log::LogSubmitStatus;
using aegisflow::log::Logger;
using aegisflow::log::LoggerConfig;
using aegisflow::log::LoggerState;
using aegisflow::log::LoggerStatus;
using aegisflow::test::require;

LoggerConfig loggerConfig(
    std::filesystem::path file,
    const LogLevel level = LogLevel::Info,
    const std::size_t queue_capacity = 4096,
    const std::chrono::milliseconds flush_interval =
        std::chrono::milliseconds(200)
) {
    LoggerConfig config;
    config.level = level;
    config.file = std::move(file);
    config.queue_capacity = queue_capacity;
    config.flush_interval = flush_interval;
    return config;
}

class TempDirectory final {
public:
    explicit TempDirectory(const std::string_view purpose) {
        path_ = std::filesystem::temp_directory_path() /
                ("aegisflow-logger-" + std::string(purpose) + '-' +
                 std::to_string(static_cast<long long>(::getpid())));
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
        require(
            std::filesystem::create_directories(path_),
            "无法创建 Logger 测试目录"
        );
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

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "无法读取 Logger 输出文件");
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
}

[[nodiscard]] std::size_t occurrenceCount(
    const std::string_view text,
    const std::string_view pattern
) {
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(pattern, position)) !=
           std::string_view::npos) {
        ++count;
        position += pattern.size();
    }
    return count;
}

void runIsolated(
    const std::string_view name,
    const std::function<void()>& scenario
) {
    const pid_t child = ::fork();
    require(child >= 0, "fork Logger 测试子进程失败");
    if (child == 0) {
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

    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        require(errno == EINTR, "waitpid Logger 测试子进程失败");
    }
    require(
        WIFEXITED(status) && WEXITSTATUS(status) == 0,
        std::string(name) + " 子进程失败"
    );
}

void concurrentProducersPreserveOwnOrder() {
    runIsolated("multi-producer", [] {
        constexpr int kProducerCount = 4;
        constexpr int kRecordsPerProducer = 100;
        const TempDirectory temporary("multi-producer");
        const auto output = temporary.path() / "nested" / "records.log";
        const LoggerConfig config = loggerConfig(
            output, LogLevel::Info, 512, 10ms);

        auto& logger = Logger::instance();
        require(logger.init(config) == LoggerStatus::Ok, "Logger 必须初始化");
        require(logger.init(config) == LoggerStatus::Ok, "相同 Logger 配置可重复 init");
        auto conflict = config;
        conflict.queue_capacity = 513;
        require(
            logger.init(conflict) == LoggerStatus::ConfigConflict,
            "不同 Logger 配置必须报冲突"
        );
        require(logger.start() == LoggerStatus::Ok, "Logger 必须启动");
        require(logger.start() == LoggerStatus::Ok, "Logger 启动必须幂等");
        require(
            logger.log(LogLevel::Debug, "filtered-debug", __FILE__, __LINE__) ==
                LogSubmitStatus::Filtered,
            "低于阈值的日志必须在入队前过滤"
        );

        std::atomic<bool> begin{false};
        std::atomic<bool> submit_failed{false};
        std::vector<std::thread> producers;
        for (int producer = 0; producer < kProducerCount; ++producer) {
            producers.emplace_back([&, producer] {
                while (!begin.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (int sequence = 0; sequence < kRecordsPerProducer;
                     ++sequence) {
                    const auto message =
                        "producer=" + std::to_string(producer) +
                        " sequence=" + std::to_string(sequence);
                    if (logger.log(
                            LogLevel::Info, message, __FILE__, __LINE__) !=
                        LogSubmitStatus::Accepted) {
                        submit_failed.store(true, std::memory_order_relaxed);
                    }
                }
            });
        }
        begin.store(true, std::memory_order_release);
        for (auto& producer : producers) {
            producer.join();
        }
        require(
            !submit_failed.load(std::memory_order_relaxed),
            "大容量队列不应丢弃测试记录"
        );

        logger.stop();
        require(logger.join() == LoggerStatus::Ok, "Logger 必须排空并 join");
        const auto stats = logger.stats();
        require(
            stats.accepted == kProducerCount * kRecordsPerProducer &&
                stats.dropped == 0,
            "Logger 接受与丢弃计数必须与提交结果一致"
        );

        const auto contents = readFile(output);
        require(
            contents.find("filtered-debug") == std::string::npos,
            "被过滤日志不得写文件"
        );
        std::array<int, kProducerCount> next_sequence{};
        std::size_t line_begin = 0;
        while (line_begin < contents.size()) {
            const auto line_end = contents.find('\n', line_begin);
            const std::string_view line(
                contents.data() + line_begin,
                (line_end == std::string::npos ? contents.size() : line_end) -
                    line_begin
            );
            for (int producer = 0; producer < kProducerCount; ++producer) {
                const auto marker =
                    "producer=" + std::to_string(producer) + " sequence=";
                const auto marker_at = line.find(marker);
                if (marker_at == std::string_view::npos) {
                    continue;
                }
                const auto value_at = marker_at + marker.size();
                const int sequence = std::stoi(
                    std::string(line.substr(value_at))
                );
                require(
                    sequence == next_sequence[producer],
                    "同一产生者的 Logger 记录必须保持提交顺序"
                );
                ++next_sequence[producer];
            }
            if (line_end == std::string::npos) {
                break;
            }
            line_begin = line_end + 1;
        }
        for (const int count : next_sequence) {
            require(
                count == kRecordsPerProducer,
                "每个产生者的全部记录必须落盘"
            );
        }
    });
}

void fullQueueDropsNewRecordAndWritesSummary() {
    runIsolated("queue-full", [] {
        const TempDirectory temporary("queue-full");
        const auto fifo = temporary.path() / "logger.fifo";
        require(
            ::mkfifo(fifo.c_str(), 0600) == 0,
            "无法创建 Logger 测试 FIFO"
        );
        const int reader_fd = ::open(
            fifo.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC
        );
        require(reader_fd >= 0, "无法打开 Logger FIFO 读端");
        const int pipe_capacity = ::fcntl(reader_fd, F_GETPIPE_SZ);
        require(pipe_capacity > 0, "无法读取 FIFO 容量");

        auto& logger = Logger::instance();
        require(
            logger.init(loggerConfig(fifo, LogLevel::Debug, 2, 1s)) ==
                LoggerStatus::Ok,
            "FIFO Logger 必须初始化"
        );
        require(logger.start() == LoggerStatus::Ok, "FIFO Logger 必须启动");

        const std::string large_message(
            static_cast<std::size_t>(pipe_capacity) * 4U,
            'X'
        );
        require(
            logger.log(LogLevel::Info, large_message, __FILE__, __LINE__) ==
                LogSubmitStatus::Accepted,
            "阻塞 worker 的大记录必须先入队"
        );

        const auto wait_deadline = std::chrono::steady_clock::now() + 3s;
        bool writer_blocked = false;
        while (std::chrono::steady_clock::now() < wait_deadline) {
            int readable = 0;
            require(
                ::ioctl(reader_fd, FIONREAD, &readable) == 0,
                "读取 FIFO 已用字节失败"
            );
            if (readable >= pipe_capacity) {
                writer_blocked = true;
                break;
            }
            std::this_thread::yield();
        }
        require(writer_blocked, "Logger worker 必须确定阻塞在已满 FIFO");

        require(
            logger.log(LogLevel::Info, "queued-1", __FILE__, __LINE__) ==
                LogSubmitStatus::Accepted &&
            logger.log(LogLevel::Info, "queued-2", __FILE__, __LINE__) ==
                LogSubmitStatus::Accepted,
            "worker 阻塞后应能精确填满两个队列槽"
        );
        require(
            logger.log(LogLevel::Error, "must-drop", __FILE__, __LINE__) ==
                LogSubmitStatus::QueueFull,
            "队列满时必须统一丢弃新记录"
        );
        require(logger.stats().dropped == 1, "丢弃计数必须立即可观测");

        std::string captured;
        std::atomic<bool> read_failed{false};
        std::thread reader([&] {
            std::array<char, 8192> buffer{};
            while (true) {
                const auto count = ::read(
                    reader_fd, buffer.data(), buffer.size()
                );
                if (count > 0) {
                    captured.append(
                        buffer.data(), static_cast<std::size_t>(count)
                    );
                    continue;
                }
                if (count == 0) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    std::this_thread::yield();
                    continue;
                }
                read_failed.store(true, std::memory_order_relaxed);
                break;
            }
        });

        logger.stop();
        require(logger.join() == LoggerStatus::Ok, "FIFO Logger 必须排空");
        reader.join();
        (void)::close(reader_fd);

        require(
            !read_failed.load(std::memory_order_relaxed),
            "排空 Logger FIFO 不得失败"
        );
        require(
            occurrenceCount(captured, "dropped_count=1") == 1,
            "Logger worker 必须在下次成功写入后输出唯一丢弃摘要"
        );
        require(
            captured.find("must-drop") == std::string::npos,
            "被丢弃的新记录不得出现在输出中"
        );
    });
}

void fileOpenAndRuntimeIoFailuresAreVisible() {
    runIsolated("file-open-failure", [] {
        const TempDirectory temporary("file-open-failure");
        auto& logger = Logger::instance();
        require(
            logger.init(loggerConfig(temporary.path())) == LoggerStatus::Ok,
            "无效文件目标不影响 init"
        );
        require(
            logger.start() == LoggerStatus::FileOpenFailed,
            "把目录当作日志文件时 start 必须失败"
        );
        require(logger.state() == LoggerState::Failed, "打开失败必须进入 Failed");
        logger.stop();
        require(
            logger.join() == LoggerStatus::Ok,
            "无 worker 的启动失败也必须可安全清理"
        );
    });

    runIsolated("runtime-io-failure", [] {
        int error_pipe[2] = {-1, -1};
        require(::pipe(error_pipe) == 0, "无法创建 stderr 捕获管道");
        const int saved_stderr = ::dup(STDERR_FILENO);
        require(saved_stderr >= 0, "无法保存 stderr");
        require(
            ::dup2(error_pipe[1], STDERR_FILENO) >= 0,
            "无法重定向 stderr"
        );
        (void)::close(error_pipe[1]);

        auto& logger = Logger::instance();
        require(
            logger.init(loggerConfig(
                "/dev/full", LogLevel::Debug, 8, 10ms)) ==
                LoggerStatus::Ok,
            "/dev/full Logger 必须初始化"
        );
        require(
            logger.start() == LoggerStatus::Ok,
            "/dev/full 可打开，启动阶段应成功"
        );
        require(
            logger.log(LogLevel::Error, "force-flush", __FILE__, __LINE__) ==
                LogSubmitStatus::Accepted,
            "ERROR 记录必须接受并强制 flush"
        );
        logger.stop();
        require(logger.join() == LoggerStatus::Ok, "I/O 失败不应阻止 join");
        require(
            logger.stats().io_errors >= 1,
            "运行期 write/flush 失败必须计数"
        );

        require(
            ::dup2(saved_stderr, STDERR_FILENO) >= 0,
            "无法恢复 stderr"
        );
        (void)::close(saved_stderr);

        std::string captured;
        std::array<char, 256> buffer{};
        while (true) {
            const auto count = ::read(
                error_pipe[0], buffer.data(), buffer.size()
            );
            if (count > 0) {
                captured.append(
                    buffer.data(), static_cast<std::size_t>(count)
                );
                continue;
            }
            require(count == 0 || errno == EINTR, "读取 stderr 捕获失败");
            if (count == 0) {
                break;
            }
        }
        (void)::close(error_pipe[0]);
        require(
            occurrenceCount(
                captured, "AegisFlow Logger: log file write failed") == 1,
            "运行期 I/O 错误只能向 stderr 提示一次"
        );
    });
}

void stopRejectsAndConcurrentJoinIsIdempotent() {
    runIsolated("stop-join", [] {
        const TempDirectory temporary("stop-join");
        const auto output = temporary.path() / "shutdown.log";
        auto& logger = Logger::instance();
        require(
            logger.init(loggerConfig(
                output, LogLevel::Debug, 32, 1s)) == LoggerStatus::Ok &&
            logger.start() == LoggerStatus::Ok,
            "停机 Logger 场景必须启动"
        );
        for (int index = 0; index < 20; ++index) {
            require(
                logger.log(
                    LogLevel::Info,
                    "accepted-before-stop=" + std::to_string(index),
                    __FILE__,
                    __LINE__
                ) == LogSubmitStatus::Accepted,
                "stop 前记录必须被接受"
            );
        }

        logger.stop();
        logger.stop();
        require(
            logger.log(LogLevel::Info, "after-stop", __FILE__, __LINE__) ==
                LogSubmitStatus::NotRunning,
            "stop 返回后不得再接受新记录"
        );

        std::atomic<bool> begin{false};
        LoggerStatus first = LoggerStatus::InvalidState;
        LoggerStatus second = LoggerStatus::InvalidState;
        std::thread first_joiner([&] {
            while (!begin.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            first = logger.join();
        });
        std::thread second_joiner([&] {
            while (!begin.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            second = logger.join();
        });
        begin.store(true, std::memory_order_release);
        first_joiner.join();
        second_joiner.join();

        require(
            first == LoggerStatus::Ok && second == LoggerStatus::Ok,
            "并发 join 必须共享同一完成结果"
        );
        logger.stop();
        require(
            logger.join() == LoggerStatus::Ok,
            "重复 stop/join 必须立即幂等返回"
        );
        require(logger.state() == LoggerState::Stopped, "Logger 必须停在 Stopped");

        const auto contents = readFile(output);
        require(
            occurrenceCount(contents, "accepted-before-stop=") == 20,
            "join 必须排空 stop 前已接受的全部记录"
        );
        require(
            contents.find("after-stop") == std::string::npos,
            "stop 后拒绝的记录不得落盘"
        );
    });
}

}  // namespace

int main() {
    return aegisflow::test::runModule(
        "async_logger",
        {
            {"多产生者顺序与级别过滤", concurrentProducersPreserveOwnOrder},
            {"确定性队满与丢弃摘要", fullQueueDropsNewRecordAndWritesSummary},
            {"文件打开与运行期 I/O 失败", fileOpenAndRuntimeIoFailuresAreVisible},
            {"stop 拒绝与重复并发 join", stopRejectsAndConcurrentJoinIsIdempotent},
        }
    );
}
