#!/usr/bin/env bash
#
# Round-trips the S3 node's checkpoint (issue #456): probes 01-05 and 15
# must be byte-identical to native. For each probe: qbe_emit_probe compiles
# it and emits QBE .ssa text plus the RtFunctionDesc triple (id/arity/
# chunkHash); qbe assembles it; cc links the assembled code, the fixed
# tools/qbe_rt_smoke/s3_harness.cpp driver, and the real libloxrt.a (not a
# throwaway stand-in) into one binary; running it and diffing its stdout
# against `build/loxpp probe.lox` is the checkpoint itself.
#
# Build with the release preset first (AGENTS.md): the debug preset leaves
# LOXPP_DEBUG_PRINT_CODE/LOXPP_DEBUG_TRACE_EXECUTION on by default, so
# native's own stdout would carry a bytecode trace no QBE-compiled binary
# ever produces, and every probe would "diverge" even though nothing is
# wrong.
#
#   cmake --preset release && cmake --build build \
#       --target loxpp qbe_emit_probe loxrt -j$(nproc)
#   tools/check_qbe_s3_straight_line.sh
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
loxpp_bin="${LOXPP_BIN:-$root/build/loxpp}"
qbe_emit_probe="${QBE_EMIT_PROBE:-$root/build/qbe_emit_probe}"
libloxrt="${LOX_RT_A:-$root/build/libloxrt.a}"
harness_src="$root/tools/qbe_rt_smoke/s3_harness.cpp"

for bin in "$loxpp_bin" "$qbe_emit_probe"; do
    if [ ! -x "$bin" ]; then
        echo "check_qbe_s3_straight_line: no executable at $bin (build it first)" >&2
        exit 1
    fi
done
if [ ! -f "$libloxrt" ]; then
    echo "check_qbe_s3_straight_line: $libloxrt not found (build it first)" >&2
    exit 1
fi

probes=(01_assign_local 02_if_else 03_and_or 04_while 05_for 15_nested_arith)

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

echo "== S3 checkpoint: straight-line code and jumps (probes 01-05, 15) =="

c++ -std=c++20 -I "$root/src" -c "$harness_src" -o harness.o

failures=0
for p in "${probes[@]}"; do
    src="$root/test/translation-probes/$p.lox"
    if [ ! -f "$src" ]; then
        echo "  FAIL  $p (no such probe: $src)"
        failures=$((failures + 1))
        continue
    fi

    if ! "$qbe_emit_probe" "$src" lox_fn_0 >"$p.ssa" 2>"$p.desc"; then
        echo "  FAIL  $p (qbe_emit_probe)"
        sed 's/^/          /' "$p.desc"
        failures=$((failures + 1))
        continue
    fi
    read -r id arity hash <"$p.desc"

    if ! qbe -o "$p.s" "$p.ssa" 2>"$p.qbe_err"; then
        echo "  FAIL  $p (qbe)"
        sed 's/^/          /' "$p.qbe_err"
        failures=$((failures + 1))
        continue
    fi

    if ! cc "$p.s" harness.o "$libloxrt" -lstdc++ -lm -o "${p}_bin" 2>"$p.cc_err"; then
        echo "  FAIL  $p (cc)"
        sed 's/^/          /' "$p.cc_err"
        failures=$((failures + 1))
        continue
    fi

    qbe_out="$(./"${p}_bin" "$src" "$id" "$arity" "$hash")" || qbe_status=$?
    qbe_status="${qbe_status:-0}"
    native_out="$("$loxpp_bin" "$src")" || native_status=$?
    native_status="${native_status:-0}"

    if [ "$qbe_out" = "$native_out" ] && [ "$qbe_status" = "$native_status" ]; then
        printf '  ok    %s\n' "$p"
    else
        printf '  FAIL  %s\n' "$p"
        printf '          qbe    (exit %s): %s\n' "$qbe_status" "$qbe_out"
        printf '          native (exit %s): %s\n' "$native_status" "$native_out"
        failures=$((failures + 1))
    fi
    unset qbe_status native_status
done

echo
if [ "$failures" -ne 0 ]; then
    echo "$failures S3 checkpoint check(s) failed."
    exit 1
fi
echo "S3 checkpoint passed."
