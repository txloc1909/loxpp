#!/usr/bin/env bash
#
# Guards the bootstrap interpreter's remaining native call-frame budget.
# bootstrap/loxpp_interpreter.lox runs on the native VM, so a target
# program's recursion borrows native's own frames through the interpreter's
# evaluator (see docs/reference/bootstrap-stack-depth.md, issue #248). Any refactor
# of the interpreter that adds frames per evaluator cycle shrinks that
# budget silently. This check keeps the margin.
#
# One check, release-only (the debug preset's instruction trace floods the
# wrapper's stdout, so no trace build can run it). The call depth itself is
# implementation-defined (spec/README.md, Conformance), so this check keeps
# only the margin that the example corpus needs, not a ceiling per
# recursion shape:
#
# 1. Corpus sweep: every examples/*.lox with CHECK lines must still pass
#    with 256 extra native frames burned before interpret(). 256 is the
#    whole pre-F10 budget kept in reserve. The padding comes from the
#    LOXPP_BOOTSTRAP_PAD hook in bootstrap/loxpp_interpreter.lox, one
#    plain native frame per level. This reuses tools/check_examples.py,
#    so a failure shows the normal PASS/FAIL lines.
# On a corpus failure the script binary-searches the failing example's
# real headroom (smallest PAD that breaks it) so the report says how far
# below 256 it sits. A failure already at PAD 0 is not a headroom
# problem and is reported as such.
#
# Usage: tools/check_bootstrap_headroom.sh
# Exits 0 when the check holds, 1 otherwise.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
wrapper="$repo_root/bootstrap/lox_wrapper.sh"

PAD_THRESHOLD=256
failures=0

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# --- Check 0: the padding hook is live -------------------------------------
# Without this, a dead __pad hook (or a runner that drops the variable)
# would let check 1 pass with no padding at all: a false green. PAD=2000
# exceeds FRAMES_MAX for any trivial program, so it must fail.

echo "### hook check: padding must be able to break a trivial program"
printf 'print 42;\n' > "$work/hook.lox"
if LOXPP_BOOTSTRAP_PAD=2000 LANGUAGE=LOXPP "$wrapper" "$work/hook.lox" >/dev/null 2>&1; then
    echo "FAIL: LOXPP_BOOTSTRAP_PAD=2000 still exits 0: the __pad hook is dead" >&2
    failures=$((failures + 1))
else
    echo "OK: the padding hook is live"
fi

# --- Check 1: corpus sweep under padding -----------------------------------

echo "### corpus sweep: examples/ with LOXPP_BOOTSTRAP_PAD=$PAD_THRESHOLD"
if LOXPP_BOOTSTRAP_PAD="$PAD_THRESHOLD" LANGUAGE=LOXPP \
    python3 "$repo_root/tools/check_examples.py" "$wrapper" "$repo_root/examples/"; then
    echo "OK: corpus passes with $PAD_THRESHOLD extra frames"
else
    echo "FAIL: corpus fails with $PAD_THRESHOLD extra frames" >&2
    failures=$((failures + 1))
    # Report each failing example's real headroom. Runs only on the
    # failure path, so the passing case stays at one corpus run.
    for prog in "$repo_root"/examples/*.lox; do
        grep -q '// CHECK:' "$prog" || continue
        if LOXPP_BOOTSTRAP_PAD="$PAD_THRESHOLD" LANGUAGE=LOXPP "$wrapper" "$prog" >/dev/null 2>&1; then
            continue
        fi
        name="$(basename "$prog")"
        if LOXPP_BOOTSTRAP_PAD=0 LANGUAGE=LOXPP "$wrapper" "$prog" >/dev/null 2>&1; then
            lo=0; hi=$PAD_THRESHOLD; first_fail=$((PAD_THRESHOLD + 1))
            while [ "$lo" -le "$hi" ]; do
                mid=$(((lo + hi) / 2))
                if LOXPP_BOOTSTRAP_PAD="$mid" LANGUAGE=LOXPP "$wrapper" "$prog" >/dev/null 2>&1; then
                    lo=$((mid + 1))
                else
                    first_fail=$mid; hi=$((mid - 1))
                fi
            done
            echo "  headroom $first_fail: $name (needs >= $PAD_THRESHOLD)" >&2
        else
            echo "  $name fails even unpadded: not a headroom problem" >&2
        fi
    done
fi

if [ "$failures" -ne 0 ]; then
    echo "$failures bootstrap headroom check(s) failed" >&2
    exit 1
fi
echo "OK: corpus sweep holds"
