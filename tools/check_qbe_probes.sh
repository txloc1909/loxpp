#!/usr/bin/env bash
#
# S7 checkpoint (issue #460): the QBE backend's own probe-and-corpus gate,
# the QBE twin of tools/check_jvm_probes.sh. Runs
# tools/loxpp_qbe.sh's stdout against build/loxpp's, byte for byte, over:
#
#   - every probe directly in test/translation-probes/ (the same directory
#     the JVM CI step walks);
#   - the QBE-only probes in test/translation-probes/qbe-only/, named
#     explicitly (S4's own list-free adaptations of V1/V3, plus fib and two
#     deep-recursion probes S4's own checkpoint already used);
#   - test/translation-probes/jvm-only/61_operator_overload.lox, named
#     explicitly: this is the operator-overloading mission's own
#     comprehensive protocol probe (every dunder method, every
#     Runtime::ResultCheck branch). It lives outside the shared corpus, but
#     QBE fully supports operator overloading (S7's own fix for the
#     dispatchMethod()-onto-a-compiled-callee hazard on #460), so this probe
#     belongs in the QBE gate.
#   - test/coroutine-probes/: mission #523's coroutine corpus. QBE compiles
#     YIELD natively (resume blocks, #530/#535), so every probe must match
#     native byte for byte.
#
# No exclusion file: unlike the JVM (LinkedHashMap), the
# QBE backend reuses native's own Table implementation unchanged (Layer 0,
# notes/qbe-backend.md), so map iteration order is identical to native by
# construction — a QBE/native diff has no permutation-only case to excuse.
#
# Requires build/loxpp, build/qbe_emit_program, and build/libloxrt.a already
# built with the release preset (AGENTS.md's warning on the debug preset's
# trace output applies to every comparison this script makes).
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
native_bin="${LOXPP_BIN:-$root/build/loxpp}"

echo "== S7 checkpoint: QBE probes (native vs QBE, release preset) =="

failures=0

run_diff() {
    local label="$1"
    shift
    echo
    echo "--- $label ---"
    if ! python3 "$root/tools/diff_runtimes.py" "$native_bin" \
             "$root/tools/loxpp_qbe.sh" "$@"; then
        failures=$((failures + 1))
    fi
}

run_diff "test/translation-probes/ (main directory)" \
    "$root/test/translation-probes/"

run_diff "test/translation-probes/qbe-only/" \
    "$root/test/translation-probes/qbe-only/"

run_diff "jvm-only/61_operator_overload.lox (operator overloading, QBE-supported)" \
    "$root/test/translation-probes/jvm-only/61_operator_overload.lox"

run_diff "test/coroutine-probes/ (native YIELD via resume blocks)" \
    "$root/test/coroutine-probes/"

echo
if [ "$failures" -ne 0 ]; then
    echo "$failures probe group(s) failed."
    exit 1
fi
echo "S7 probe checkpoint passed."
