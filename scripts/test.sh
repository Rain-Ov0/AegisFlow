#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${TMPDIR:-/tmp}/aegisflow-build-${UID}/tests-debug}"
PARALLEL="${PARALLEL:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"

readonly MODULE_TARGETS=(
    test_array_view
    test_cancellation
    test_app_config
    test_length_prefixed_codec
    test_login_protocol
    test_login_request_validator
    test_login_policy
    test_blacklist_candidate_generator
    test_blacklist_candidate_queue
    test_login_business_handler
    test_sliding_window
    test_feature_store
    test_feature_state_maintenance
    test_benchmark_metrics
    test_async_logger
    test_mysql_blacklist
    test_redis_blacklist_store
    test_blacklist_cache_bootstrap
    test_blacklist_maintenance
    test_manage_blacklist
    test_benchmark_reset
    test_bounded_worker_pool
    test_timer
    test_session
    test_event_loop
    test_lifecycle
)

usage() {
    echo "usage: $0 <module|all>" >&2
    echo "modules:" >&2
    printf '  %s\n' "${MODULE_TARGETS[@]#test_}" >&2
}

if [[ $# -ne 1 ]]; then
    usage
    exit 2
fi

requested="$1"
cmake -S "${REPO_DIR}" -B "${BUILD_DIR}" \
    -DAEGISFLOW_BUILD_PROJECT=OFF \
    -DAEGISFLOW_BUILD_BENCHMARK=OFF \
    -DAEGISFLOW_BUILD_TESTS=ON \
    -DCMAKE_BUILD_TYPE=Debug

if [[ "${requested}" == "all" ]]; then
    cmake --build "${BUILD_DIR}" \
        --parallel "${PARALLEL}" \
        --target "${MODULE_TARGETS[@]}"
    ctest --test-dir "${BUILD_DIR}" --output-on-failure
    exit 0
fi

target="${requested}"
if [[ "${target}" != test_* ]]; then
    target="test_${target}"
fi

known=false
for candidate in "${MODULE_TARGETS[@]}"; do
    if [[ "${candidate}" == "${target}" ]]; then
        known=true
        break
    fi
done
if [[ "${known}" != true ]]; then
    echo "unknown test module: ${requested}" >&2
    usage
    exit 2
fi

cmake --build "${BUILD_DIR}" --parallel "${PARALLEL}" --target "${target}"
ctest --test-dir "${BUILD_DIR}" \
    --output-on-failure \
    --tests-regex "^${target}($|_integration$)"
