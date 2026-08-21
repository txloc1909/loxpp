#!/usr/bin/env bash
#
# tools/loxpp_clr.sh's stdout on each probe must be identical to build/loxpp's
# own stdout, byte for byte. Compares with diff, not by eye. The CLR twin of
# tools/check_jvm_probes.sh, scoped today to the straight-line opcode set
# (CONSTANT, NIL/TRUE/FALSE, arithmetic/comparison, NEGATE, NOT, PRINT, POP,
# GET_LOCAL, SET_LOCAL, DEFINE_GLOBAL, GET_GLOBAL, SET_GLOBAL, script-form
# RETURN) — later CLR backend work grows this list the same way
# check_jvm_probes.sh grew as the JVM backend gained opcodes.
#
# Every probe runs even after an earlier one fails: a failing CLR run (an
# ilasm assembly error, say) is caught and recorded as this probe's own
# failure, not left to `set -e` at top level, which would otherwise stop the
# loop at the first failure and hide every later probe.
#
# Requires build/loxpp (LOXPP_CLR_BACKEND, default ON) and
# runtime/clr/LoxRuntime.dll (tools/build_lox_rt_clr.sh) already built.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
native_bin="${LOXPP_BIN:-$root/build/loxpp}"

probes=(
    "notes/translation-probes/01_assign_local.lox"
    "notes/translation-probes/15_nested_arith.lox"
    "notes/translation-probes/18_peek_of_named_local.lox"
    "notes/translation-probes/19_peek_of_named_local_global.lox"
    "notes/translation-probes/20_float_imprecise_constant.lox"
    "notes/translation-probes/21_exponent_constant.lox"
)

if [ ! -x "$native_bin" ]; then
    echo "check_clr_probes.sh: no loxpp binary at $native_bin" >&2
    exit 1
fi

native_out="$(mktemp)"
native_err="$(mktemp)"
clr_out="$(mktemp)"
clr_err="$(mktemp)"
trap 'rm -f "$native_out" "$native_err" "$clr_out" "$clr_err"' EXIT

failed_probes=()
for probe in "${probes[@]}"; do
    if ! "$native_bin" "$root/$probe" >"$native_out" 2>"$native_err"; then
        echo "check_clr_probes.sh: FAIL $probe (native run failed)" >&2
        cat "$native_err" >&2
        failed_probes+=("$probe")
        continue
    fi
    if ! "$root/tools/loxpp_clr.sh" "$root/$probe" >"$clr_out" 2>"$clr_err"; then
        echo "check_clr_probes.sh: FAIL $probe (CLR run failed)" >&2
        cat "$clr_err" >&2
        failed_probes+=("$probe")
        continue
    fi
    if diff -u "$native_out" "$clr_out"; then
        echo "check_clr_probes.sh: OK $probe"
    else
        echo "check_clr_probes.sh: FAIL $probe (stdout mismatch)" >&2
        failed_probes+=("$probe")
    fi
done

if [ "${#failed_probes[@]}" -ne 0 ]; then
    echo "check_clr_probes.sh: ${#failed_probes[@]} probe(s) failed:" >&2
    for probe in "${failed_probes[@]}"; do
        echo "  $probe" >&2
    done
    exit 1
fi

echo "check_clr_probes.sh: all ${#probes[@]} probes OK"
