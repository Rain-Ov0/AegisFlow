#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-8080}"
REQUESTS="${REQUESTS:-20000}"
THREADS="${THREADS:-8}"
ATTACK_RATIO="${ATTACK_RATIO:-0.20}"
ROUNDS="${ROUNDS:-1}"
OUTPUT_DIR="${OUTPUT_DIR:-logs/pressure}"

mkdir -p "${OUTPUT_DIR}"

if [[ ! -x "${BUILD_DIR}/benchmark_http" ]]; then
    cmake -S . -B "${BUILD_DIR}"
    cmake --build "${BUILD_DIR}" --target benchmark_http
fi

for arg in "$@"; do
    if [[ "${arg}" == "--help" ]]; then
        "${BUILD_DIR}/benchmark_http" --help
        exit 0
    fi
done

for ((round = 1; round <= ROUNDS; ++round)); do
    ts="$(date +%Y%m%d_%H%M%S)"
    output="${OUTPUT_DIR}/http_${ts}_round${round}.log"

    "${BUILD_DIR}/benchmark_http" \
        --host "${HOST}" \
        --port "${PORT}" \
        --requests "${REQUESTS}" \
        --threads "${THREADS}" \
        --attack-ratio "${ATTACK_RATIO}" \
        "$@" | tee "${output}"

    echo "saved=${output}"
done
