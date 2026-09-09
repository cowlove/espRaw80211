#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/.." && pwd)
workspace=$(mktemp -d "${TMPDIR:-/tmp}/espRaw80211-log-compat.XXXXXX")
cleanup() {
    rm -rf -- "$workspace"
}
trap cleanup EXIT

split_and_analyze() {
    local directory=$1
    awk -v directory="$directory" '
        /^ddeeff00000[1-7] / {
            board = substr($1, length($1), 1) - 1
            print > (directory "/cat.usb" board ".out")
        }
    ' "$directory/all.log"
    "$script_dir/analyze_rendezvous.py" \
        --log-dir "$directory" --local-only --session all --tail-bytes -1 \
        --evidence --overlaps --scout-links --json > "$directory/analysis.json"
}

mkdir "$workspace/legacy" "$workspace/compact"
"$script_dir/run_csim.sh" --seconds 360 --random-seed 41 \
    --legacy-diagnostics > "$workspace/legacy/all.log" 2>/dev/null
"$script_dir/run_csim.sh" --seconds 360 --random-seed 41 \
    > "$workspace/compact/all.log" 2>/dev/null

if ! grep -Eq '@q b=[0-9a-f]{12} r=-?[0-9]+ n=[1-9][0-9]* a=[0-9]+ t=[0-9]+' \
        "$workspace/compact/all.log"; then
    echo 'compact CSIM log is missing a valid end-of-wake @q beacon record' >&2
    exit 1
fi

split_and_analyze "$workspace/legacy"
split_and_analyze "$workspace/compact"

if ! cmp -s "$workspace/legacy/analysis.json" \
          "$workspace/compact/analysis.json"; then
    diff -u "$workspace/legacy/analysis.json" \
            "$workspace/compact/analysis.json"
    exit 1
fi

legacy_bytes=$(wc -c < "$workspace/legacy/all.log")
compact_bytes=$(wc -c < "$workspace/compact/all.log")
printf 'CSIM log compatibility passed: analysis identical; %d -> %d bytes\n' \
    "$legacy_bytes" "$compact_bytes"
