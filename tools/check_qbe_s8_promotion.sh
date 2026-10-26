#!/usr/bin/env bash
#
# S8 checkpoint (issue #461): register promotion's own correctness gate.
#
# Promotion narrows the set of values the GC can see on the QBE backend's
# fused stack: a promoted local lives in a QBE register, not its stack cell,
# between safe points. This runs the native-vs-QBE differential under
# LOXPP_STRESS_GC=1, where every allocation collects, so a promoted local a
# safe point failed to spill is freed while still live and the run diverges.
#
# It covers:
#   - every probe in test/translation-probes/ (the main directory and
#     qbe-only/, which holds the s8_promotion_* probes this node added);
#   - test/translation-probes/jvm-only/61_operator_overload.lox, which QBE
#     supports (see tools/check_qbe_probes.sh);
#   - the whole examples/ corpus.
#
# The whole example corpus needs longer than the 30s default under stress GC,
# so --timeout 180 is used.
#
# Requires build/loxpp, build/qbe_emit_program, and build/libloxrt.a already
# built with the release preset (AGENTS.md's warning on the debug preset's
# trace output applies to every comparison this script makes).
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
native_bin="${LOXPP_BIN:-$root/build/loxpp}"

echo "== S8 checkpoint: register promotion under LOXPP_STRESS_GC=1 =="

failures=0

run_diff() {
    local label="$1"
    shift
    echo
    echo "--- $label ---"
    if ! LOXPP_STRESS_GC=1 python3 "$root/tools/diff_runtimes.py" "$native_bin" \
             "$root/tools/loxpp_qbe.sh" "$@"; then
        failures=$((failures + 1))
    fi
}

run_diff "test/translation-probes/ + qbe-only/ (stress GC)" \
    "$root/test/translation-probes/" \
    "$root/test/translation-probes/qbe-only/"

run_diff "jvm-only/61_operator_overload.lox (stress GC)" \
    "$root/test/translation-probes/jvm-only/61_operator_overload.lox"

run_diff "examples/ (stress GC)" \
    "$root/examples/" --timeout 180

echo
if [ "$failures" -ne 0 ]; then
    echo "$failures group(s) failed under LOXPP_STRESS_GC=1."
    exit 1
fi
echo "S8 promotion checkpoint passed."
