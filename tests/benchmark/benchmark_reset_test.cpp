#include "tests/support/test_harness.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using aegisflow::test::require;

class TempDirectory final {
public:
    TempDirectory() {
        const auto suffix = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
        path_ = std::filesystem::temp_directory_path() /
                ("aegisflow-benchmark-reset-" + std::to_string(suffix));
        std::filesystem::create_directories(path_);
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

std::string shellQuote(const std::string& value) {
    std::string quoted = "'";
    for (const char character : value) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(character);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

void writeExecutable(
    const std::filesystem::path& path,
    const std::string_view contents
) {
    std::ofstream output(path, std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("无法创建 fake command");
    }
    output << contents;
    output.close();
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace
    );
}

std::vector<std::string> readLines(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::vector<std::string> lines;
    for (std::string line; std::getline(input, line);) {
        lines.push_back(std::move(line));
    }
    return lines;
}

int runPressure(
    const std::filesystem::path& pressure_script,
    const std::filesystem::path& config,
    const std::filesystem::path& reset,
    const std::filesystem::path& benchmark,
    const std::filesystem::path& trace,
    const std::string& scenarios = "smoke"
) {
    const std::string command =
        "TRACE_FILE=" + shellQuote(trace.string()) +
        " RESET_BIN=" + shellQuote(reset.string()) +
        " BENCHMARK_BIN=" + shellQuote(benchmark.string()) +
        " BENCHMARK_CONFIG=" + shellQuote(config.string()) +
        " ROUNDS=3 SCENARIOS=" + shellQuote(scenarios) +
        " RUN_TOKEN=reset-test "
        "bash " + shellQuote(pressure_script.string());
    return std::system(command.c_str());
}

void invalidScenarioListsStopBeforeCommands(
    const std::filesystem::path& pressure_script,
    const std::filesystem::path& config
) {
    TempDirectory fixture;
    const auto reset = fixture.path() / "reset";
    const auto benchmark = fixture.path() / "benchmark";
    const auto trace = fixture.path() / "trace";
    const auto fake =
        "#!/usr/bin/env bash\n"
        "printf 'called\\n' >> \"${TRACE_FILE}\"\n";
    writeExecutable(reset, fake);
    writeExecutable(benchmark, fake);

    require(
        runPressure(
            pressure_script, config, reset, benchmark, trace, "smoke,unknown"
        ) != 0,
        "未知场景必须在执行外部命令前拒绝"
    );
    require(
        runPressure(
            pressure_script, config, reset, benchmark, trace, ""
        ) != 0,
        "空场景列表必须在执行外部命令前拒绝"
    );
    require(
        !std::filesystem::exists(trace) || readLines(trace).empty(),
        "非法场景列表不得调用 reset 或 benchmark"
    );
}

void resetFailureStopsBeforeWarmupOrMeasurement(
    const std::filesystem::path& pressure_script,
    const std::filesystem::path& config
) {
    TempDirectory fixture;
    const auto reset = fixture.path() / "reset";
    const auto benchmark = fixture.path() / "benchmark";
    const auto trace = fixture.path() / "trace";
    writeExecutable(
        reset,
        "#!/usr/bin/env bash\n"
        "printf 'reset %s\\n' \"$*\" >> \"${TRACE_FILE}\"\n"
        "exit 17\n"
    );
    writeExecutable(
        benchmark,
        "#!/usr/bin/env bash\n"
        "printf 'benchmark\\n' >> \"${TRACE_FILE}\"\n"
        "exit 0\n"
    );

    require(
        runPressure(pressure_script, config, reset, benchmark, trace) != 0,
        "reset 失败时 pressure_test 必须失败"
    );
    const auto lines = readLines(trace);
    require(
        lines.size() == 1 &&
            lines.front() ==
                "reset clear --all --wait --confirm benchmark-reset",
        "reset 失败后不得调用 benchmark 发出 warmup 或计量请求"
    );
}

void everyRoundResetsFirstAndUsesUniqueEntities(
    const std::filesystem::path& pressure_script,
    const std::filesystem::path& config
) {
    TempDirectory fixture;
    const auto reset = fixture.path() / "reset";
    const auto benchmark = fixture.path() / "benchmark";
    const auto trace = fixture.path() / "trace";
    writeExecutable(
        reset,
        "#!/usr/bin/env bash\n"
        "printf 'reset %s\\n' \"$*\" >> \"${TRACE_FILE}\"\n"
    );
    writeExecutable(
        benchmark,
        "#!/usr/bin/env bash\n"
        "entity=''\n"
        "attack_ip=''\n"
        "while (($#)); do\n"
        "  case \"$1\" in\n"
        "    --entity-prefix) entity=\"$2\"; shift 2 ;;\n"
        "    --attack-ip) attack_ip=\"$2\"; shift 2 ;;\n"
        "    *) shift ;;\n"
        "  esac\n"
        "done\n"
        "printf 'benchmark %s %s\\n' \"${entity}\" \"${attack_ip}\" >> \"${TRACE_FILE}\"\n"
        "printf '%s\\n' \"expected_requests=1 issued_requests=1 decoded_responses=1 failed_requests=0 status_ok=1 status_overloaded=0 status_timeout=0 action_pass=1 action_review=0 action_reject=0 policy_blacklisted_user=0 policy_blacklisted_ip=0 policy_blacklisted_device=0 policy_credential_stuffing_attack=0 policy_too_many_failed_login=0 policy_ip_many_users_failed_login=0 policy_device_many_accounts=0 policy_other=0 failure_connect=0 failure_connect_timeout=0 failure_send=0 failure_read=0 failure_peer_closed=0 failure_protocol=0 failure_parse=0 failure_mismatch=0 failure_request_timeout=0 failure_internal=0 qps=1.000 p50_us=10 p95_us=10 p99_us=10 max_us=10 measurement_us=1000000 planned_reconnects=0 entity_prefix=${entity} attack_ip=${attack_ip}\"\n"
    );

    require(
        runPressure(pressure_script, config, reset, benchmark, trace) == 0,
        "fake reset 和 benchmark 成功时三轮编排必须通过"
    );
    const auto lines = readLines(trace);
    require(lines.size() == 6, "三轮必须各执行一次 reset 和 benchmark");

    std::set<std::string> entities;
    std::set<std::string> attack_ips;
    for (std::size_t round = 0; round < 3; ++round) {
        require(
            lines[round * 2] ==
                "reset clear --all --wait --confirm benchmark-reset",
            "每轮必须先成功 reset 才能执行 benchmark"
        );
        const auto& benchmark_line = lines[round * 2 + 1];
        require(
            benchmark_line.size() >= 10 &&
                benchmark_line.compare(0, 10, "benchmark ") == 0,
            "reset 后必须恰好执行一次 benchmark"
        );
        const auto separator = benchmark_line.find(' ', 10);
        require(separator != std::string::npos, "fake benchmark trace 格式错误");
        entities.insert(benchmark_line.substr(10, separator - 10));
        attack_ips.insert(benchmark_line.substr(separator + 1));
    }
    require(entities.size() == 3, "每轮 entity_prefix 必须不同");
    require(attack_ips.size() == 3, "每轮 attack_ip 必须不同");
}

}  // namespace

int main(const int argc, char* argv[]) {
    if (argc != 3) {
        throw std::invalid_argument(
            "benchmark_reset_test 需要 pressure_test.sh 和配置路径"
        );
    }
    const std::filesystem::path pressure_script(argv[1]);
    const std::filesystem::path config(argv[2]);
    return aegisflow::test::runModule(
        "benchmark_reset",
        {
            {"场景列表预校验", [&] {
                 invalidScenarioListsStopBeforeCommands(
                     pressure_script, config
                 );
             }},
            {"reset 失败短路", [&] {
                 resetFailureStopsBeforeWarmupOrMeasurement(
                     pressure_script, config
                 );
             }},
            {"逐轮 reset 与唯一实体", [&] {
                 everyRoundResetsFirstAndUsesUniqueEntities(
                     pressure_script, config
                 );
             }},
        }
    );
}
