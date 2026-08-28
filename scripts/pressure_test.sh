#!/usr/bin/env bash
set -euo pipefail

BUILD_ROOT="${BUILD_ROOT:-${TMPDIR:-/tmp}/aegisflow-build-${UID}}"
PROJECT_BUILD_DIR="${PROJECT_BUILD_DIR:-${BUILD_ROOT}/project-release}"
BENCHMARK_BUILD_DIR="${BENCHMARK_BUILD_DIR:-${BUILD_ROOT}/benchmark-release}"
BENCHMARK_BIN="${BENCHMARK_BIN:-${BENCHMARK_BUILD_DIR}/benchmark_native}"
RESET_BIN="${RESET_BIN:-${PROJECT_BUILD_DIR}/manage_blacklist}"
RESET_CONFIG="${RESET_CONFIG:-}"
BENCHMARK_CONFIG="${BENCHMARK_CONFIG:-config/benchmark_native.conf}"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-8080}"
ROUNDS="${ROUNDS:-3}"
SCENARIOS="${SCENARIOS-smoke,steady,attack,churn,overload,deadline}"
SEED="${SEED:-20260815}"
RUN_TOKEN="${RUN_TOKEN:-$(date +%s%N)-$$}"

STEADY_CONCURRENCY="${STEADY_CONCURRENCY:-8}"
STEADY_CONNECTIONS="${STEADY_CONNECTIONS:-64}"
STEADY_QPS="${STEADY_QPS:-10000}"
STEADY_DURATION_MS="${STEADY_DURATION_MS:-10000}"
STEADY_REQUESTS_PER_CONNECTION="${STEADY_REQUESTS_PER_CONNECTION:-1000000}"
ATTACK_RATIO="${ATTACK_RATIO:-0.50}"
OVERLOAD_CONCURRENCY="${OVERLOAD_CONCURRENCY:-256}"
OVERLOAD_CONNECTIONS="${OVERLOAD_CONNECTIONS:-256}"
DEADLINE_PORT="${DEADLINE_PORT:-${PORT}}"

if [[ ! "${ROUNDS}" =~ ^[0-9]+$ ]] || ((ROUNDS < 3 || ROUNDS > 65535)); then
    echo "ROUNDS 必须是 3..65535 的整数" >&2
    exit 2
fi

IFS=',' read -r -a requested_scenarios <<< "${SCENARIOS}"
if [[ -z "${SCENARIOS}" || ${#requested_scenarios[@]} -eq 0 ]]; then
    echo "SCENARIOS 不能为空" >&2
    exit 2
fi
for requested_scenario in "${requested_scenarios[@]}"; do
    case "${requested_scenario}" in
        smoke|steady|attack|churn|overload|deadline) ;;
        *) echo "未知场景: ${requested_scenario}" >&2; exit 2 ;;
    esac
done
if [[ ! -x "${BENCHMARK_BIN}" ]]; then
    echo "benchmark_native 不可执行: ${BENCHMARK_BIN}" >&2
    exit 1
fi
if [[ ! -x "${RESET_BIN}" ]]; then
    echo "reset hook 不可执行: ${RESET_BIN}" >&2
    echo "完整压力入口要求 RESET_BIN 实现 clear --all --wait --confirm benchmark-reset；当前不会绕过复位继续发流量。" >&2
    exit 1
fi
if [[ ! -r "${BENCHMARK_CONFIG}" ]]; then
    echo "benchmark 配置不可读: ${BENCHMARK_CONFIG}" >&2
    exit 1
fi

if [[ " ${SCENARIOS//,/ } " == *" overload "* ]]; then
    echo "overload 场景要求服务端 worker_pool.queue_capacity 小于并发突发量。"
fi
if [[ " ${SCENARIOS//,/ } " == *" deadline "* ]]; then
    echo "deadline 场景要求 ${HOST}:${DEADLINE_PORT} 使用 server.business_timeout_ms=1。"
fi

scenario_enabled() {
    [[ ",${SCENARIOS}," == *",$1,"* ]]
}

validate_summary() {
    local scenario="$1"
    local summary="$2"
    python3 - "${scenario}" "${summary}" <<'PY'
import math
import sys

scenario, line = sys.argv[1:]
fields = {}
for token in line.split():
    if "=" not in token:
        raise SystemExit(f"摘要存在非 key=value token: {token}")
    key, value = token.split("=", 1)
    if not key or key in fields:
        raise SystemExit(f"摘要 key 为空或重复: {key}")
    fields[key] = value

integer_keys = {
    "expected_requests", "issued_requests", "decoded_responses",
    "failed_requests", "status_ok", "status_overloaded", "status_timeout",
    "action_pass", "action_review", "action_reject", "planned_reconnects",
}
failure_keys = {
    "failure_connect", "failure_connect_timeout", "failure_send",
    "failure_read", "failure_peer_closed", "failure_protocol",
    "failure_parse", "failure_mismatch", "failure_request_timeout",
    "failure_internal",
}
policy_keys = {
    "policy_blacklisted_user", "policy_blacklisted_ip",
    "policy_blacklisted_device", "policy_credential_stuffing_attack",
    "policy_too_many_failed_login", "policy_ip_many_users_failed_login",
    "policy_device_many_accounts", "policy_other",
}
required = integer_keys | failure_keys | policy_keys | {
    "qps", "p50_us", "p95_us", "p99_us", "max_us",
    "measurement_us", "entity_prefix", "attack_ip",
}
missing = required - fields.keys()
if missing:
    raise SystemExit("摘要缺少字段: " + ",".join(sorted(missing)))

numbers = {key: int(fields[key]) for key in integer_keys | failure_keys | policy_keys}
qps = float(fields["qps"])
if not math.isfinite(qps) or qps < 0:
    raise SystemExit("qps 必须是非负有限数")

assert numbers["expected_requests"] == numbers["issued_requests"], "计划请求未全部进入终态"
assert numbers["issued_requests"] == (
    numbers["decoded_responses"] + numbers["failed_requests"]
), "issued = decoded + failed 记账不平"
assert numbers["decoded_responses"] == (
    numbers["status_ok"] + numbers["status_overloaded"] + numbers["status_timeout"]
), "decoded = status sum 记账不平"
assert numbers["status_ok"] == (
    numbers["action_pass"] + numbers["action_review"] + numbers["action_reject"]
), "status_ok = action sum 记账不平"
assert numbers["failed_requests"] == sum(numbers[key] for key in failure_keys), (
    "failed 与失败分类记账不平"
)
assert numbers["failed_requests"] == 0, "计量阶段存在失败请求"

if scenario in {"smoke", "steady", "churn"}:
    assert numbers["status_ok"] > 0, "基准场景未获得 OK 响应"
    assert numbers["status_overloaded"] == 0 and numbers["status_timeout"] == 0, (
        "基准场景出现过载或超时"
    )
if scenario == "attack":
    assert numbers["status_ok"] > 0, "攻击场景未获得 OK 响应"
    assert numbers["action_review"] + numbers["action_reject"] > 0, (
        "攻击流量未命中风险动作"
    )
    assert sum(numbers[key] for key in policy_keys) > 0, "攻击流量未产生策略命中"
if scenario == "churn":
    assert numbers["planned_reconnects"] > 0, "churn 场景未发生计划重连"
if scenario == "overload":
    assert numbers["status_overloaded"] > 0, "过载场景未观测到 OVERLOADED"
if scenario == "deadline":
    assert numbers["status_timeout"] > 0, "deadline 场景未观测到 TIMEOUT"
PY
}

print_median() {
    local scenario="$1"
    shift
    python3 - "${scenario}" "$@" <<'PY'
import statistics
import sys

scenario, *lines = sys.argv[1:]
rows = []
for line in lines:
    rows.append(dict(token.split("=", 1) for token in line.split()))

def median(key, cast=float):
    return statistics.median(cast(row[key]) for row in rows)

print(
    f"scenario={scenario} rounds={len(rows)} "
    f"median_qps={median('qps'):.3f} "
    f"median_p50_us={median('p50_us', int):g} "
    f"median_p95_us={median('p95_us', int):g} "
    f"median_p99_us={median('p99_us', int):g} "
    f"median_max_us={median('max_us', int):g}"
)
PY
}

run_scenario() {
    local scenario="$1"
    local port="$2"
    local concurrency="$3"
    local connections="$4"
    local qps="$5"
    local warmup_ms="$6"
    local duration_ms="$7"
    local attack_ratio="$8"
    local requests_per_connection="$9"
    local scenario_id=0

    case "${scenario}" in
        smoke) scenario_id=1 ;;
        steady) scenario_id=2 ;;
        attack) scenario_id=3 ;;
        churn) scenario_id=4 ;;
        overload) scenario_id=5 ;;
        deadline) scenario_id=6 ;;
        *) echo "未知场景: ${scenario}" >&2; exit 2 ;;
    esac

    local run_number
    run_number="$(printf '%s' "${RUN_TOKEN}" | cksum | awk '{print $1}')"
    local run_hex
    printf -v run_hex '%x' "$(((run_number % 65535) + 1))"
    local scenario_hex
    printf -v scenario_hex '%x' "${scenario_id}"
    local -a summaries=()
    local -a reset_arguments=()
    if [[ -n "${RESET_CONFIG}" ]]; then
        reset_arguments+=(--config "${RESET_CONFIG}")
    fi
    # reset hook 的完整契约是：
    # manage_blacklist clear --all --wait --confirm benchmark-reset
    reset_arguments+=(clear --all --wait --confirm benchmark-reset)

    for ((round = 1; round <= ROUNDS; ++round)); do
        local round_hex
        printf -v round_hex '%x' "${round}"
        local entity="${scenario}_r${run_number}_n${round}"
        local attack_ip="2001:db8:${run_hex}:${scenario_hex}::${round_hex}"

        if ! "${RESET_BIN}" "${reset_arguments[@]}"; then
            echo "${scenario} round ${round}: reset hook 失败，未发送 warmup 或计量请求" >&2
            return 1
        fi

        local summary
        local exit_code=0
        set +e
        summary="$(
            "${BENCHMARK_BIN}" \
                --config "${BENCHMARK_CONFIG}" \
                --host "${HOST}" \
                --port "${port}" \
                --request-concurrency "${concurrency}" \
                --connection-pool-size "${connections}" \
                --requests-per-connection "${requests_per_connection}" \
                --target-qps "${qps}" \
                --warmup-ms "${warmup_ms}" \
                --duration-ms "${duration_ms}" \
                --seed "$((SEED + scenario_id * 1000 + round))" \
                --attack-ratio "${attack_ratio}" \
                --attack-ip "${attack_ip}" \
                --entity-prefix "${entity}"
        )"
        exit_code=$?
        set -e

        if ((exit_code != 0)); then
            if [[ -n "${summary}" && "${summary}" != *$'\n'* ]]; then
                echo "scenario=${scenario} round=${round} ${summary}"
            fi
            echo "${scenario} round ${round}: benchmark 退出码 ${exit_code}" >&2
            return "${exit_code}"
        fi
        if [[ -z "${summary}" || "${summary}" == *$'\n'* ]]; then
            echo "${scenario} round ${round}: benchmark 必须只输出一行摘要" >&2
            return 1
        fi
        validate_summary "${scenario}" "${summary}"
        summaries+=("${summary}")
        echo "scenario=${scenario} round=${round} ${summary}"
    done

    print_median "${scenario}" "${summaries[@]}"
}

scenario_enabled smoke && run_scenario smoke "${PORT}" 1 1 20 200 1000 0 1000
scenario_enabled steady && run_scenario steady "${PORT}" \
    "${STEADY_CONCURRENCY}" "${STEADY_CONNECTIONS}" "${STEADY_QPS}" \
    1000 "${STEADY_DURATION_MS}" 0 "${STEADY_REQUESTS_PER_CONNECTION}"
scenario_enabled attack && run_scenario attack "${PORT}" \
    "${STEADY_CONCURRENCY}" "${STEADY_CONNECTIONS}" "${STEADY_QPS}" \
    1000 "${STEADY_DURATION_MS}" "${ATTACK_RATIO}" \
    "${STEADY_REQUESTS_PER_CONNECTION}"
scenario_enabled churn && run_scenario churn "${PORT}" 8 32 1000 500 3000 0 1
scenario_enabled overload && run_scenario overload "${PORT}" \
    "${OVERLOAD_CONCURRENCY}" "${OVERLOAD_CONNECTIONS}" 0 0 1000 0 20
scenario_enabled deadline && run_scenario deadline "${DEADLINE_PORT}" \
    16 32 5000 500 3000 0.20 1000

exit 0
