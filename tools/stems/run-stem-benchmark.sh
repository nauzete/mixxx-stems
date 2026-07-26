#!/usr/bin/env bash

set -euo pipefail

if [[ $# -lt 2 || $# -gt 4 ]]; then
    echo "Usage: $0 <demucs-onnx-benchmark> <htdemucs.onnx> [output-dir] [threads]" >&2
    exit 2
fi

benchmark_bin="$(realpath "$1")"
model_path="$(realpath "$2")"
output_dir="${3:-stem-benchmark-$(date -u +%Y%m%dT%H%M%SZ)}"
thread_count="${4:-2}"

if [[ ! -x "${benchmark_bin}" ]]; then
    echo "Benchmark executable is not executable: ${benchmark_bin}" >&2
    exit 2
fi
if [[ ! -f "${model_path}" ]]; then
    echo "Model does not exist: ${model_path}" >&2
    exit 2
fi
if [[ ! "${thread_count}" =~ ^[1-9][0-9]*$ ]]; then
    echo "Thread count must be a positive integer" >&2
    exit 2
fi

mkdir -p "${output_dir}"

{
    echo "timestamp_utc=$(date -u --iso-8601=seconds)"
    echo "uname=$(uname -a)"
    echo "architecture=$(uname -m)"
    echo "threads=${thread_count}"
    lscpu
    free -h
    swapon --show
    lsblk -o NAME,TYPE,SIZE,ROTA,TRAN,MOUNTPOINTS
    if command -v vcgencmd >/dev/null 2>&1; then
        vcgencmd measure_temp
        vcgencmd get_throttled
    fi
} >"${output_dir}/system.txt"

if [[ -x /usr/bin/time ]]; then
    /usr/bin/time -v \
        -o "${output_dir}/time.txt" \
        "${benchmark_bin}" \
        "${model_path}" \
        "${thread_count}" \
        "${output_dir}/benchmark.json"
else
    "${benchmark_bin}" \
        "${model_path}" \
        "${thread_count}" \
        "${output_dir}/benchmark.json"
fi

echo "Benchmark evidence written to ${output_dir}"
