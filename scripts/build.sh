#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_ROOT="${BUILD_ROOT:-${TMPDIR:-/tmp}/aegisflow-build-${UID}}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
PARALLEL="${PARALLEL:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"

usage() {
    echo "usage: $0 [project|benchmark]" >&2
}

group="${1:-project}"
case "${group}" in
    project)
        build_dir="${BUILD_DIR:-${BUILD_ROOT}/project-${BUILD_TYPE,,}}"
        targets=(AegisFlow send_event manage_blacklist)
        project=ON
        benchmark=OFF
        ;;
    benchmark)
        build_dir="${BUILD_DIR:-${BUILD_ROOT}/benchmark-${BUILD_TYPE,,}}"
        targets=(benchmark_native benchmark_feature_store)
        project=OFF
        benchmark=ON
        ;;
    *)
        usage
        exit 2
        ;;
esac

cmake -S "${REPO_DIR}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DAEGISFLOW_BUILD_PROJECT="${project}" \
    -DAEGISFLOW_BUILD_BENCHMARK="${benchmark}" \
    -DAEGISFLOW_BUILD_TESTS=OFF
cmake --build "${build_dir}" \
    --parallel "${PARALLEL}" \
    --target "${targets[@]}"

echo "build_group=${group} build_dir=${build_dir}"
