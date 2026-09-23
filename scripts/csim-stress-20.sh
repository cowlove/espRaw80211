#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/.." && pwd)
binary=espRaw80211_csim_20

make -C "$project_dir" BOARD=csim CONTEXT_COUNT=20 \
    CSIM_BINARY="$binary" "$binary"

# This is deliberately a periodic, expensive stress test.  Callers may
# override any benchmark option, for example --iterations 20 for a smoke run.
CSIM_BINARY="$binary" "$script_dir/csim-benchmark.sh" \
    --iterations 2000 --seconds 36000 "$@"
