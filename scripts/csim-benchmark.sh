#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
runner="$script_dir/run_csim.sh"

iterations=200
timeout_seconds=3600
reception_scale=0.60
jobs=$(nproc)

results_dir=$(mktemp -d "${TMPDIR:-/tmp}/espRaw80211-benchmark.XXXXXX")
results="$results_dir/times"
cleanup() {
    rm -rf -- "$results_dir"
}
trap cleanup EXIT

echo "CSIM first-convergence benchmark"
echo "  runs: $iterations"
echo "  parallel jobs: $jobs"
echo "  simulated timeout: ${timeout_seconds}s"
echo "  reception scale: $reception_scale"

for seed in $(seq 1 "$iterations"); do
    (
        "$runner" \
            --seconds "$timeout_seconds" \
            --reception-scale "$reception_scale" \
            --random-seed "$seed" \
            --exit-on-convergence 2>/dev/null |
            awk '
                /CSIM GLOBAL CONVERGENCE/ {
                    for (i = 1; i <= NF; i++) {
                        if ($i ~ /^time=/) {
                            sub(/^time=/, "", $i)
                            print $i
                            exit
                        }
                    }
                }
            ' > "$results_dir/$seed"
    ) &

    if (( seed % jobs == 0 )); then
        wait
    fi
done
wait

cat "$results_dir"/[0-9]* > "$results"

sort -n -o "$results" "$results"

awk -v total="$iterations" '
    { values[NR] = $1; sum += $1 }
    END {
        successes = NR
        timeouts = total - successes

        printf "\nResults\n"
        printf "  converged: %d/%d\n", successes, total
        printf "  timeouts:  %d\n", timeouts

        if (successes == 0) {
            exit
        }

        median = successes % 2 \
            ? values[(successes + 1) / 2] \
            : (values[successes / 2] + values[successes / 2 + 1]) / 2
        p95_index = int(successes * 0.95 + 0.999999)

        printf "  min:       %.3fs\n", values[1]
        printf "  median:    %.3fs\n", median
        printf "  mean:      %.3fs\n", sum / successes
        printf "  p95:       %.3fs\n", values[p95_index]
        printf "  max:       %.3fs\n", values[successes]
    }
' "$results"
