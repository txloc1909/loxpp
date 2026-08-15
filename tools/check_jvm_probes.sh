#!/usr/bin/env bash
#
# Node N4/N5 checkpoint: tools/loxpp_jvm.sh's stdout on each probe must be
# identical to build/loxpp's own stdout, byte for byte. Compares with diff,
# not by eye (notes/backend-implementation-dag.md, N4/N5). Later emission
# nodes add their own probes to this same list as they add opcodes.
#
# Every probe runs even after an earlier one fails: a failing JVM run (a
# verifier rejection, say) is caught and recorded as this probe's own
# failure, not left to `set -e` at top level, which would otherwise stop the
# loop at the first failure and hide every later probe (N5.md — PR #107 R11,
# left open at merge).
#
# Requires build/loxpp (LOXPP_JVM_BACKEND, default ON) and
# runtime/jvm/lox-rt.jar (tools/build_lox_rt.sh) already built.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
native_bin="${LOXPP_BIN:-$root/build/loxpp}"

probes=(
    "notes/translation-probes/01_assign_local.lox"
    "notes/translation-probes/02_if_else.lox"
    "notes/translation-probes/03_and_or.lox"
    "notes/translation-probes/04_while.lox"
    "notes/translation-probes/05_for.lox"
    "notes/translation-probes/15_nested_arith.lox"
    "notes/translation-probes/18_peek_of_named_local.lox"
    "notes/translation-probes/19_peek_of_named_local_global.lox"
    "notes/translation-probes/20_float_imprecise_constant.lox"
    "notes/translation-probes/21_exponent_constant.lox"
    "notes/translation-probes/22_and_or_assignment_statement.lox"
    "notes/translation-probes/23_and_or_local_initializer.lox"
)

if [ ! -x "$native_bin" ]; then
    echo "check_jvm_probes.sh: no loxpp binary at $native_bin" >&2
    exit 1
fi

native_out="$(mktemp)"
native_err="$(mktemp)"
jvm_out="$(mktemp)"
jvm_err="$(mktemp)"
trap 'rm -f "$native_out" "$native_err" "$jvm_out" "$jvm_err"' EXIT

failed_probes=()
for probe in "${probes[@]}"; do
    if ! "$native_bin" "$root/$probe" >"$native_out" 2>"$native_err"; then
        echo "check_jvm_probes.sh: FAIL $probe (native run failed)" >&2
        cat "$native_err" >&2
        failed_probes+=("$probe")
        continue
    fi
    if ! "$root/tools/loxpp_jvm.sh" "$root/$probe" >"$jvm_out" 2>"$jvm_err"; then
        echo "check_jvm_probes.sh: FAIL $probe (JVM run failed)" >&2
        cat "$jvm_err" >&2
        failed_probes+=("$probe")
        continue
    fi
    if diff -u "$native_out" "$jvm_out"; then
        echo "check_jvm_probes.sh: OK $probe"
    else
        echo "check_jvm_probes.sh: FAIL $probe (stdout mismatch)" >&2
        failed_probes+=("$probe")
    fi
done

if [ "${#failed_probes[@]}" -ne 0 ]; then
    echo "check_jvm_probes.sh: ${#failed_probes[@]} probe(s) failed:" >&2
    for probe in "${failed_probes[@]}"; do
        echo "  $probe" >&2
    done
    exit 1
fi

echo "check_jvm_probes.sh: all ${#probes[@]} probes OK"
