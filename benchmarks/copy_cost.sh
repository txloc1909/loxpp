#!/usr/bin/env bash
#
# Builds and runs benchmarks/copy_bench.cpp, the message-copy measurement
# behind the concurrency decision. Two phases, like coroutine_cost_qbe.sh:
#   benchmarks/copy_cost.sh compile [BUILD]            (in a dev image)
#   benchmarks/copy_cost.sh run [BUILD] [CORE] [N] [R] (host, pinned)
# The compile links build/libloxrt.a from a release build
# (`cmake --preset release && cmake --build build --target loxrt`).
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
case "${1:-}" in
compile)
    build="${2:-build}"
    clang++ -std=c++20 -O3 -DNDEBUG -Isrc benchmarks/copy_bench.cpp \
        "$build/libloxrt.a" -lpthread -o "$build/copy_bench"
    echo "built $build/copy_bench"
    ;;
run)
    build="${2:-build}"; core="${3:-2}"; n="${4:-100000}"; reps="${5:-7}"
    echo "copy_bench ($(git -C "$root" rev-parse --short HEAD)), core $core, N $n, R $reps"
    echo "governor: $(cat /sys/devices/system/cpu/cpu$core/cpufreq/scaling_governor 2>/dev/null)  epp: $(cat /sys/devices/system/cpu/cpu$core/cpufreq/energy_performance_preference 2>/dev/null)  ac: $(cat /sys/class/power_supply/AC*/online 2>/dev/null)"
    taskset -c "$core" "$build/copy_bench" "$n" "$reps"
    ;;
*)
    echo "usage: $0 compile [BUILD] | run [BUILD] [CORE] [N] [R]" >&2; exit 2;;
esac
