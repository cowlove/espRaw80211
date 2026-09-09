#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/.." && pwd)
csim_binary="$project_dir/espRaw80211_csim"

if [[ ! -x "$csim_binary" ]]; then
    echo "CSIM binary not found; run: make BOARD=csim espRaw80211_csim" >&2
    exit 1
fi

state_dir=$(mktemp -d "${TMPDIR:-/tmp}/espRaw80211-csim.XXXXXX")
cleanup() {
    rm -rf -- "$state_dir"
}
trap cleanup EXIT

echo "CSIM isolated state: $state_dir" >&2
cd -- "$state_dir"
"$csim_binary" "$@"
