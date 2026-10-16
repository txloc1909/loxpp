#!/usr/bin/env bash
#
# The QBE half of the measurements issue #590 asks for: the same programs as
# benchmarks/coroutine_cost.sh, compiled with `--target qbe`, plus the
# coroutine-mode tax on a program that never suspends.
#
# Two phases, because the QBE toolchain lives in the dev-qbe image and the
# timing wants the host's cores and GNU time:
#   benchmarks/coroutine_cost_qbe.sh compile [BINARY] [OUTDIR]   (in dev-qbe)
#   benchmarks/coroutine_cost_qbe.sh run [OUTDIR] [CORE] [WARM] [MEAS]   (host)
# Each program is compiled once; the executables embed the source and need
# nothing beside them at run time.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
programs=(coroutine_yield coroutine_call_baseline coroutine_mode_floor coroutine_mode_tax coroutine_yield_depth coroutine_footprint)

phase="${1:-}"
case "$phase" in
compile)
    bin="${2:-build/loxpp}"; out="${3:-build/qbe-bench}"
    mkdir -p "$out"
    for p in "${programs[@]}"; do
        "$bin" --target qbe -o "$out/$p" "$root/benchmarks/$p.lox"
        echo "compiled $p"
    done
    ;;
run)
    out="${2:-build/qbe-bench}"; core="${3:-2}"; warm="${4:-5}"; meas="${5:-10}"
    median() { sort -n | awk '{a[NR]=$1} END {print (NR%2) ? a[(NR+1)/2] : (a[NR/2]+a[NR/2+1])/2}'; }
    timed() {
        local exe="$1"; shift; local vals=()
        for _ in $(seq "$warm"); do taskset -c "$core" "$exe" "$@" >/dev/null; done
        for _ in $(seq "$meas"); do vals+=("$(taskset -c "$core" "$exe" "$@")"); done
        printf '%s\n' "${vals[@]}" | tr '\n' ' '; echo
        printf 'median %s\n' "$(printf '%s\n' "${vals[@]}" | median)"
    }
    maxrss_kb() { /usr/bin/time -f '%M' taskset -c "$core" "$@" 2>&1 >/dev/null | tail -1; }
    echo "executables: $out ($(git -C "$root" rev-parse --short HEAD)), core $core, warm $warm, meas $meas"
    echo "governor: $(cat /sys/devices/system/cpu/cpu$core/cpufreq/scaling_governor 2>/dev/null)  epp: $(cat /sys/devices/system/cpu/cpu$core/cpufreq/energy_performance_preference 2>/dev/null)  ac: $(cat /sys/class/power_supply/AC*/online 2>/dev/null)"
    echo
    echo "== per-yield cost, ns per round trip, coroutine mode =="
    timed "$out/coroutine_yield"
    echo "== plain call floor, ns per call, normal mode (no YIELD in the program) =="
    timed "$out/coroutine_call_baseline"
    echo "== call with local state, ns per call, normal mode =="
    timed "$out/coroutine_mode_floor"
    echo "== call with local state, ns per call, coroutine mode (an unreached YIELD in the program) =="
    timed "$out/coroutine_mode_tax"
    echo
    echo "== per-yield cost against depth, coroutine mode =="
    for d in 1 10 100; do printf 'depth %-4s' "$d"; timed "$out/coroutine_yield_depth" "$d" | tail -1; done
    echo
    echo "== bytes per suspended coroutine (max RSS delta / N) =="
    for depth in 1 10; do
        base=$(maxrss_kb "$out/coroutine_footprint" 0 "$depth")
        for n in 10000 50000; do
            rss=$(maxrss_kb "$out/coroutine_footprint" "$n" "$depth")
            echo "depth $depth N $n: rss ${rss} KB, base ${base} KB, $(( (rss - base) * 1024 / n )) bytes per coroutine"
        done
    done
    ;;
*)
    echo "usage: $0 compile [BINARY] [OUTDIR] | run [OUTDIR] [CORE] [WARM] [MEAS]" >&2; exit 2;;
esac
