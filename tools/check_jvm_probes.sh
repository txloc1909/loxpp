#!/usr/bin/env bash
#
# Node N4 checkpoint: tools/loxpp_jvm.sh's stdout on each straight-line probe
# must be identical to build/loxpp's own stdout, byte for byte. Compares with
# diff, not by eye (notes/backend-implementation-dag.md, N4). Later emission
# nodes add their own probes to this same list as they add opcodes.
#
# Requires build/loxpp (LOXPP_JVM_BACKEND, default ON) and
# runtime/jvm/lox-rt.jar (tools/build_lox_rt.sh) already built.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
native_bin="${LOXPP_BIN:-$root/build/loxpp}"

probes=(
    "notes/translation-probes/01_assign_local.lox"
    "notes/translation-probes/15_nested_arith.lox"
    "notes/translation-probes/18_peek_of_named_local.lox"
    "notes/translation-probes/19_peek_of_named_local_global.lox"
)

if [ ! -x "$native_bin" ]; then
    echo "check_jvm_probes.sh: no loxpp binary at $native_bin" >&2
    exit 1
fi

native_out="$(mktemp)"
jvm_out="$(mktemp)"
trap 'rm -f "$native_out" "$jvm_out"' EXIT

fail=0
for probe in "${probes[@]}"; do
    "$native_bin" "$root/$probe" > "$native_out"
    "$root/tools/loxpp_jvm.sh" "$root/$probe" > "$jvm_out"
    if diff -u "$native_out" "$jvm_out"; then
        echo "check_jvm_probes.sh: OK $probe"
    else
        echo "check_jvm_probes.sh: MISMATCH $probe" >&2
        fail=1
    fi
done

exit "$fail"
