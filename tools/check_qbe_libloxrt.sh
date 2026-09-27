#!/usr/bin/env bash
#
# Round-trips the S2 node's checkpoint: a hand-written .ssa file that prints
# a string and calls a stdlib function, assembled and linked against the
# real libloxrt.a (not the throwaway stand-in tools/check_qbe_toolchain.sh
# uses). Run this after building libloxrt.a — it does not build it itself,
# so a missing library fails fast with a clear message instead of a
# confusing qbe/cc error.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
libloxrt="${1:-$repo_root/build/libloxrt.a}"
fixture="$repo_root/tools/qbe_rt_smoke/s2_hello.ssa"

if [ ! -f "$libloxrt" ]; then
    echo "check_qbe_libloxrt: $libloxrt not found." >&2
    echo "Build it first, e.g.: cmake --preset release && cmake --build build --target loxrt" >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cp "$fixture" "$work/s2_hello.ssa"
cd "$work"

failures=0

check() {
    local name="$1" expected="$2" actual="$3"
    if [ "$actual" = "$expected" ]; then
        printf '  ok    %s\n' "$name"
    else
        printf '  FAIL  %s\n          expected: %s\n          actual:   %s\n' \
            "$name" "$expected" "$actual"
        failures=$((failures + 1))
    fi
}

step() {
    local name="$1"; shift
    local out
    if out="$("$@" 2>&1)"; then
        return 0
    fi
    printf '  FAIL  %s\n' "$name"
    printf '%s\n' "$out" | sed 's/^/          /'
    failures=$((failures + 1))
    return 1
}

echo "== S2 checkpoint: libloxrt.a + startup =="

if step "qbe" qbe -o s2_hello.s s2_hello.ssa \
    && step "cc (assemble + link)" cc s2_hello.s "$libloxrt" -lstdc++ -lm -o s2_hello; then
    out="$(./s2_hello)" && status=0 || status=$?
    check "s2_hello exit code" "0" "$status"
    check "s2_hello stdout" "hello from qbe" "$out"
fi

echo
if [ "$failures" -ne 0 ]; then
    echo "$failures S2 checkpoint check(s) failed."
    exit 1
fi
echo "S2 checkpoint passed."
