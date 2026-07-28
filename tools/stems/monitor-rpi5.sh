#!/usr/bin/env bash

set -euo pipefail

if [[ $# -lt 1 || $# -gt 4 ]]; then
    echo "Usage: $0 <mixxx-pid> [duration-seconds] [interval-seconds] [output-dir]" >&2
    exit 2
fi

mixxx_pid="$1"
duration_seconds="${2:-7200}"
interval_seconds="${3:-5}"
output_dir="${4:-rpi5-soak-$(date -u +%Y%m%dT%H%M%SZ)}"
settings_root="${MIXXX_STEMS_SETTINGS_ROOT:-${HOME}/.mixxx-stems/stems}"
mixxx_log="${MIXXX_STEMS_LOG:-}"

for value in "${mixxx_pid}" "${duration_seconds}" "${interval_seconds}"; do
    if [[ ! "${value}" =~ ^[1-9][0-9]*$ ]]; then
        echo "PID, duration, and interval must be positive integers" >&2
        exit 2
    fi
done
if [[ ! -r "/proc/${mixxx_pid}/status" ]]; then
    echo "Process ${mixxx_pid} is not readable" >&2
    exit 2
fi

mkdir -p "${output_dir}"
csv="${output_dir}/telemetry.csv"
echo "timestamp_utc,rss_kib,available_kib,swap_free_kib,cpu_percent,temp_millic,throttled,cache_bytes,queue_jobs,underruns" >"${csv}"

deadline=$((SECONDS + duration_seconds))
while ((SECONDS < deadline)); do
    if [[ ! -r "/proc/${mixxx_pid}/status" ]]; then
        echo "Mixxx process exited before the soak test completed" >&2
        exit 1
    fi

    rss_kib="$(awk '/^VmRSS:/ {print $2}' "/proc/${mixxx_pid}/status")"
    available_kib="$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)"
    swap_free_kib="$(awk '/^SwapFree:/ {print $2}' /proc/meminfo)"
    cpu_percent="$(ps -p "${mixxx_pid}" -o %cpu= | tr -d ' ')"
    temp_millic="$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo 0)"
    if command -v vcgencmd >/dev/null 2>&1; then
        throttled="$(vcgencmd get_throttled | cut -d= -f2)"
    else
        throttled="unavailable"
    fi
    cache_bytes="$(du -sb "${settings_root}/cache" 2>/dev/null | awk '{print $1}')"
    cache_bytes="${cache_bytes:-0}"
    if [[ -r "${settings_root}/queue.json" ]] && command -v jq >/dev/null 2>&1; then
        queue_jobs="$(jq '.jobs | length' "${settings_root}/queue.json")"
    else
        queue_jobs=0
    fi
    if [[ -n "${mixxx_log}" && -r "${mixxx_log}" ]]; then
        underruns="$(grep -Eic 'underrun|xrun' "${mixxx_log}" || true)"
    else
        underruns=0
    fi

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$(date -u --iso-8601=seconds)" \
        "${rss_kib:-0}" \
        "${available_kib:-0}" \
        "${swap_free_kib:-0}" \
        "${cpu_percent:-0}" \
        "${temp_millic}" \
        "${throttled}" \
        "${cache_bytes}" \
        "${queue_jobs}" \
        "${underruns}" >>"${csv}"

    sleep "${interval_seconds}"
done

if command -v pw-top >/dev/null 2>&1; then
    pw-top -b -n 1 >"${output_dir}/pipewire-final.txt" 2>&1 || true
fi
free -h >"${output_dir}/memory-final.txt"
swapon --show >"${output_dir}/swap-final.txt"
echo "Soak telemetry written to ${output_dir}"
