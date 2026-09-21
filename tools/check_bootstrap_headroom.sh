#!/usr/bin/env bash
#
# Guards the bootstrap interpreter's remaining native call-frame budget.
# bootstrap/loxpp_interpreter.lox runs on the native VM, so a target
# program's recursion borrows native's own frames through the interpreter's
# evaluator (see notes/bootstrap-stack-depth.md, issue #248). Any refactor
# of the interpreter that adds frames per evaluator cycle shrinks that
# budget silently. This check keeps the margin.
#
# Two checks, both release-only (the debug preset's instruction trace
# floods the wrapper's stdout, so no trace build can run them):
#
# 1. Corpus sweep: every examples/*.lox with CHECK lines must still pass
#    with 256 extra native frames burned before interpret(). 256 is the
#    whole pre-F10 budget kept in reserve. The padding comes from the
#    LOXPP_BOOTSTRAP_PAD hook in bootstrap/loxpp_interpreter.lox, one
#    plain native frame per level. This reuses tools/check_examples.py,
#    so a failure shows the normal PASS/FAIL lines.
# 2. Shape floors: the seven recursion-shape probes from the
#    research/bootstrap-depth-evidence branch must each run clean at 90%
#    of their post-F10 ceilings. The sweep cannot see a one-frame-per-
#    cycle change at 1024 frames (it moves parser.lox by only ~40
#    frames), but the same change moves the tail ceiling from 168 to
#    about 146, past its floor of 151.
#
# On a corpus failure the script binary-searches the failing example's
# real headroom (smallest PAD that breaks it) so the report says how far
# below 256 it sits. A failure already at PAD 0 is not a headroom
# problem and is reported as such.
#
# Usage: tools/check_bootstrap_headroom.sh
# Exits 0 when both checks hold, 1 otherwise.
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

# --- Check 2: recursion-shape floors ----------------------------------------
# Each probe: program, floor depth, expected stdout. The probes count the
# depth they actually reach in a global, so the expected output proves the
# floor depth ran, not just that the program exited 0. Floors are 90% of
# the post-F10 ceilings measured on the release preset (tail 168, method
# 168, binary 125, match 126, block 100, ctor 100, super 83).

run_shape() { # $1 name, $2 expected stdout, $3 program text
    local name="$1" expected="$2" prog="$3"
    local f="$work/$name.lox"
    printf '%s\n' "$prog" > "$f"
    # A failing probe must set status, not kill the script: under set -e
    # an assignment's command substitution inherits the failure.
    local out status=0
    out=$(env -u LOXPP_BOOTSTRAP_PAD LANGUAGE=LOXPP "$wrapper" "$f" 2>&1) || status=$?
    if [ "$status" -ne 0 ]; then
        echo "FAIL: shape $name exits $status (floor not reached): $out" >&2
        failures=$((failures + 1))
        return
    fi
    if [ "$out" != "$expected" ]; then
        echo "FAIL: shape $name prints '$out', expected '$expected'" >&2
        failures=$((failures + 1))
        return
    fi
    if grep -q '] in ' <<<"$out"; then
        echo "FAIL: shape $name leaks a native traceback line" >&2
        failures=$((failures + 1))
        return
    fi
    echo "PASS: shape $name reaches its floor"
}

echo "### shape floors: seven recursion shapes, unpadded"
run_shape "tail" "151" \
'var seen = 0;
fun f(n) { if (n > seen) seen = n; if (n == 0) return 0; return f(n - 1); }
f(151);
print seen;'
run_shape "method" "151" \
'var seen = 0;
class C { m(n) { if (n > seen) seen = n; if (n == 0) return 0; return this.m(n - 1); } }
C().m(151);
print seen;'
run_shape "binary" "112" \
'var seen = 0;
fun f(n) { if (n > seen) seen = n; if (n == 0) return 0; return f(n - 1) + 1; }
print f(112);'
run_shape "match" "113" \
'var seen = 0;
fun f(n) { if (n > seen) seen = n; return match n { case 0 => 0 case _ => f(n - 1) }; }
f(113);
print seen;'
run_shape "block" "90" \
'var seen = 0;
fun f(n) { if (n > seen) seen = n; if (n > 0) { return f(n - 1); } return 0; }
f(90);
print seen;'
run_shape "ctor" "90" \
'var seen = 0;
class C { init(n) { if (n > seen) seen = n; if (n > 0) { C(n - 1); } } }
C(90);
print seen;'
run_shape "super" "74" \
'var seen = 0;
class A { m(n) { if (n > seen) seen = n; if (n == 0) return 0; return this.m(n - 1); } }
class B < A { m(n) { return super.m(n); } }
B().m(74);
print seen;'

if [ "$failures" -ne 0 ]; then
    echo "$failures bootstrap headroom check(s) failed" >&2
    exit 1
fi
echo "OK: corpus sweep and 7 shape floors all hold"
