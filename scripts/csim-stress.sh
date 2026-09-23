#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/.." && pwd)
devices=20
benchmark_arguments=()

while (( $# )); do
    case $1 in
        --devices)
            devices=$2
            shift 2
            ;;
        --devices=*)
            devices=${1#*=}
            shift
            ;;
        *)
            benchmark_arguments+=("$1")
            shift
            ;;
    esac
done

if [[ ! $devices =~ ^[1-9][0-9]*$ ]] || (( devices > 255 )); then
    echo "--devices must be an integer from 1 through 255" >&2
    exit 2
fi

binary="espRaw80211_csim_${devices}"
make -C "$project_dir" BOARD=csim CONTEXT_COUNT="$devices" \
    CSIM_BINARY="$binary" "$binary"

CSIM_BINARY="$binary" "$script_dir/csim-benchmark.sh" \
    --iterations 2000 --seconds 36000 "${benchmark_arguments[@]}"
