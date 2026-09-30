#!/usr/bin/env bash
#
# Round-trips a trivial program through the QBE toolchain in the dev-qbe
# image. Its purpose is diagnostic isolation, the same reason
# check_managed_toolchains.sh exists: once a QBE-backed --target qbe exists,
# a failure here means the image is broken or version-skewed, and a failure
# there means the generated .ssa is wrong. Without it, the first backend
# commit debugs both at once.
set -euo pipefail

fixtures="$(cd "$(dirname "${BASH_SOURCE[0]}")/toolchain_smoke" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

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

# Compile/assemble/link steps have to report through the same path as
# everything else. As bare commands under `set -e` they abort the script
# before `check` runs, so a broken toolchain would surface as a bare nonzero
# exit with no explanation. Output is captured and printed only on failure.
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

cd "$work"
cp "$fixtures/hello.ssa" "$fixtures/bad.ssa" "$fixtures/hello_rt.cpp" .

echo "== toolchain versions =="
printf '  %s\n' "$(cat /opt/qbe/VERSION 2>/dev/null || echo '<no VERSION file found>')"
printf '  cc %s\n' "$(cc --version 2>&1 | head -1 || true)"

echo
echo "== round-trip =="

# 1. QBE lowers the hand-written .ssa to target assembly.
if step "qbe" qbe -o hello.s hello.ssa; then
    # 2. The C++ static library stands in for libloxrt.a: the same shape a
    #    real backend links its generated code against.
    if step "c++ (runtime lib)" c++ -std=c++17 -c hello_rt.cpp -o hello_rt.o \
        && step "ar (runtime lib)" ar rcs libhello_rt.a hello_rt.o; then
        # 3. cc assembles hello.s and links it against the static library,
        #    the assemble+link half of the pattern jvm_run.sh/ilasm already
        #    prove for the JVM backend.
        if step "cc (assemble + link)" cc hello.s libhello_rt.a -lstdc++ -o hello; then
            hello_out="$(./hello 2>&1)" && hello_status=0 || hello_status=$?
            check "qbe -> cc -> run (exit code)" "0" "$hello_status"
            check "qbe -> cc -> run (stdout)" "qbe ok" "$hello_out"
        fi
    fi
fi

# 4. Malformed-IL rejection: proves QBE's own parser/type-checker still
# catches bad input on this image, the QBE counterpart of
# check_managed_toolchains.sh's stack-depth-mismatch check. A pass here means
# QBE still rejects; qbe exiting 0 on this fixture is the failure.
bad_out="$(qbe -o bad.s bad.ssa 2>&1)" && bad_status=0 || bad_status=$?
if [ "$bad_status" -ne 0 ]; then
    printf '  ok    qbe rejects malformed IL (undefined temporary)\n'
else
    printf '  FAIL  qbe rejects malformed IL (undefined temporary)\n'
    printf '          expected: nonzero exit\n'
    printf '          exit:     %s\n' "$bad_status"
    printf '%s\n' "$bad_out" | sed 's/^/          actual:   /'
    failures=$((failures + 1))
fi

echo
if [ "$failures" -ne 0 ]; then
    echo "$failures QBE toolchain check(s) failed."
    exit 1
fi
echo "All QBE toolchain checks passed."
