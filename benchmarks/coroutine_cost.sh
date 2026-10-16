#!/usr/bin/env bash
#
# The measurements issue #590 (the concurrency go/no-go) asks for, in one run:
#   1. per-yield cost: benchmarks/coroutine_yield.lox against the plain-call
#      floor benchmarks/coroutine_call_baseline.lox;
#   2. bytes per suspended coroutine: benchmarks/coroutine_footprint.lox at
#      two suspension depths, from the max resident set of the process.
# Every timed run is pinned to one core. The warm-up runs are discarded; the
# measured runs are printed and summarised by their median.
#
# Usage: benchmarks/coroutine_cost.sh [BINARY] [CORE] [WARM] [MEAS]
set -euo pipefail

bin="${1:-build/loxpp}"
core="${2:-2}"
warm="${3:-5}"
meas="${4:-10}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

median() { sort -n | awk '{a[NR]=$1} END {print (NR%2) ? a[(NR+1)/2] : (a[NR/2]+a[NR/2+1])/2}'; }

timed() { # program -> prints the measured values, then "median <v>"
    local prog="$1" vals=()
    for _ in $(seq "$warm"); do taskset -c "$core" "$bin" "$prog" >/dev/null; done
    for _ in $(seq "$meas"); do vals+=("$(taskset -c "$core" "$bin" "$prog")"); done
    printf '%s\n' "${vals[@]}" | tr '\n' ' '; echo
    printf 'median %s\n' "$(printf '%s\n' "${vals[@]}" | median)"
}

maxrss_kb() { # program args... -> max resident set in KB
    /usr/bin/time -f '%M' taskset -c "$core" "$bin" "$@" 2>&1 >/dev/null | tail -1
}

echo "binary: $bin ($(git -C "$root" rev-parse --short HEAD)), core $core, warm $warm, meas $meas"
echo "governor: $(cat /sys/devices/system/cpu/cpu$core/cpufreq/scaling_governor 2>/dev/null)  epp: $(cat /sys/devices/system/cpu/cpu$core/cpufreq/energy_performance_preference 2>/dev/null)  ac: $(cat /sys/class/power_supply/AC*/online 2>/dev/null)"
echo
echo "== per-yield cost, ns per round trip (resume + yield) =="
timed benchmarks/coroutine_yield.lox
echo "== plain call floor, ns per call and return =="
timed benchmarks/coroutine_call_baseline.lox
echo
echo "== bytes per suspended coroutine (max RSS delta / N) =="
for depth in 1 10; do
    base=$(maxrss_kb benchmarks/coroutine_footprint.lox 0 "$depth")
    for n in 10000 50000; do
        rss=$(maxrss_kb benchmarks/coroutine_footprint.lox "$n" "$depth")
        echo "depth $depth N $n: rss ${rss} KB, base ${base} KB, $(( (rss - base) * 1024 / n )) bytes per coroutine"
    done
done
