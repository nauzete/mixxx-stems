#!/usr/bin/env bash

set -euo pipefail

output_file="${1:-hardware-checks-$(date -u +%Y%m%dT%H%M%SZ).tsv}"
checks=(
    "DDJ-FLX4 detected and mapping loaded"
    "Deck 1 stem pads and LEDs"
    "Deck 2 stem pads and LEDs"
    "Long-hold generation and cancellation"
    "Master output"
    "Headphone cue output"
    "10.1-inch touch layout at 800x480"
    "microSD workload"
    "SSD workload"
    "No sustained swap"
    "No audio underruns"
    "No thermal throttling"
    "Two-hour soak test"
)

printf 'timestamp_utc\tcheck\tresult\tnotes\n' >"${output_file}"
for check in "${checks[@]}"; do
    echo
    echo "${check}"
    read -r -p "Result [PASS/FAIL/SKIP]: " result
    result="${result^^}"
    case "${result}" in
        PASS|FAIL|SKIP)
            ;;
        *)
            echo "Invalid result" >&2
            exit 2
            ;;
    esac
    read -r -p "Notes: " notes
    notes="${notes//$'\t'/ }"
    printf '%s\t%s\t%s\t%s\n' \
        "$(date -u --iso-8601=seconds)" \
        "${check}" \
        "${result}" \
        "${notes}" >>"${output_file}"
done

echo "Hardware evidence written to ${output_file}"
