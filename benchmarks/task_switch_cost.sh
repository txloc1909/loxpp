#!/usr/bin/env bash
#
# The cost of one task switch: benchmarks/task_ping_pong.lox sends a message
# back and forth between two tasks, 1,000,000 task switches in all, with no
# park. Give two release binaries to compare them. The runs alternate (A B A B
# ...), so a drift of the CPU speed hits both binaries alike, and each run is
# pinned to one core. The figure is user plus system CPU time of the process,
# not wall time. The first runs are warm-up and are not counted.
#
# Usage: benchmarks/task_switch_cost.sh BINARY_A [BINARY_B] [CORE] [RUNS] [WARM]
set -euo pipefail

a="${1:-build/loxpp}"
b="${2:-$a}"
core="${3:-2}"
runs="${4:-15}"
warm="${5:-3}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
prog=benchmarks/task_ping_pong.lox
switches=1000000

cpu_ms() { # binary -> user + system CPU time in ms
    local t
    t=$({ TIMEFORMAT='%3U %3S'; time taskset -c "$core" "$1" "$prog" >/dev/null; } 2>&1)
    awk '{printf "%d", ($1 + $2) * 1000}' <<<"$t"
}

median() { sort -n | awk '{a[NR]=$1} END {print (NR%2) ? a[(NR+1)/2] : (a[NR/2]+a[NR/2+1])/2}'; }

for _ in $(seq "$warm"); do
    cpu_ms "$a" >/dev/null
    cpu_ms "$b" >/dev/null
done
va=() vb=()
for _ in $(seq "$runs"); do
    va+=("$(cpu_ms "$a")")
    vb+=("$(cpu_ms "$b")")
done

echo "core $core, runs $runs, warm $warm, $switches task switches per run"
for side in a b; do
    name="${!side}"
    declare -n v="v$side"
    med=$(printf '%s\n' "${v[@]}" | median)
    echo "$side: $name"
    echo "  CPU ms per run: ${v[*]}"
    echo "  median $med ms, $(awk -v m="$med" -v n="$switches" 'BEGIN {printf "%.1f", m * 1e6 / n}') ns per task switch"
done
