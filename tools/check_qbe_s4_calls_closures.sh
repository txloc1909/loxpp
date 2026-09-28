#!/usr/bin/env bash
#
# Round-trips the S4 node's checkpoint (issue #457): probes 06 and 08, the
# QBE-only V1/V3 adaptations (no BUILD_LIST/GET_INDEX/SET_INDEX — S5's job),
# V2_shared, fib, and two deep-recursion probes (Q3's own measured test)
# must be byte-identical to native (stdout + exit status).
# deep_recursion.lox trips Runtime::call's pre-existing FRAMES_MAX guard;
# deep_recursion_wide.lox keeps 20 locals live per frame, so it trips the
# new per-function STACK_MAX check (emitStackCheck, qbe_emitter.cpp)
# before FRAMES_MAX can fire (found in review, PR #481).
#
# Unlike S3's checkpoint (one function per probe, one fixed harness symbol),
# an S4 probe compiles a WHOLE program: the top-level script plus every
# function CLOSURE/CALL reach, however deep. qbe_emit_program (this node's
# own checkpoint driver, tools/qbe_emit_program.cpp) emits one QBE .ssa
# translation unit for the whole tree, plus one "<id> <arity> <chunkHash>
# <qbeSymbol>" line per function on stderr. This script turns those lines
# into a small, per-probe harness.cpp — one `extern "C"` declaration and one
# RtFunctionDesc array entry per function — compiled and linked alongside
# the assembled code and libloxrt.a. rt_startup is called with
# requireAllCompiled=true (rt_capi.h): every function in the program must
# have code attached, or the whole-program invariant this node's own hazard
# fix (R4 on issue #457) exists to enforce would go unchecked.
#
# Build with the release preset first (AGENTS.md): the debug preset leaves
# LOXPP_DEBUG_PRINT_CODE/LOXPP_DEBUG_TRACE_EXECUTION on by default, so
# native's own stdout would carry a bytecode trace no QBE-compiled binary
# ever produces, and every probe would "diverge" even though nothing is
# wrong.
#
#   cmake --preset release && cmake --build build \
#       --target loxpp qbe_emit_program loxrt -j$(nproc)
#   tools/check_qbe_s4_calls_closures.sh
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
loxpp_bin="${LOXPP_BIN:-$root/build/loxpp}"
qbe_emit_program="${QBE_EMIT_PROGRAM:-$root/build/qbe_emit_program}"
libloxrt="${LOX_RT_A:-$root/build/libloxrt.a}"

for bin in "$loxpp_bin" "$qbe_emit_program"; do
    if [ ! -x "$bin" ]; then
        echo "check_qbe_s4_calls_closures: no executable at $bin (build it first)" >&2
        exit 1
    fi
done
if [ ! -f "$libloxrt" ]; then
    echo "check_qbe_s4_calls_closures: $libloxrt not found (build it first)" >&2
    exit 1
fi

probes=(
    06_shared_upvalue
    08_call
    qbe-only/V1_fresh_cell_no_lists
    qbe-only/V3_loopvar_no_lists
    V2_shared
    qbe-only/fib
    qbe-only/deep_recursion
    qbe-only/deep_recursion_wide
)

# deep_recursion.lox trips Runtime::call's pre-existing FRAMES_MAX guard;
# deep_recursion_wide.lox is built to trip STACK_MAX first instead. Both
# report "Stack overflow." on stderr and exit 70 (loxpp's one generic
# status for every uncaught runtime error — confirmed against native: a
# plain "not a callable value" TypeError exits 70 too), so a stdout+exit
# comparison alone cannot tell a real overflow from any other error the
# corrupted stack could produce below (found in review, PR #481, R1) — a
# regression in STACK_MAX's own check could corrupt the stack silently and
# still land on exit 70 with empty stdout, matching native by accident.
# These probes get one more check: the actual "Stack overflow." diagnostic
# text, on both sides.
overflow_diagnostic_probes=(deep_recursion deep_recursion_wide)

requires_overflow_diagnostic() {
    local candidate="$1" probe
    for probe in "${overflow_diagnostic_probes[@]}"; do
        if [ "$candidate" = "$probe" ]; then
            return 0
        fi
    done
    return 1
}

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

echo "== S4 checkpoint: calls and closures (probes 06, 08, V1/V2/V3, fib, deep recursion x2) =="

failures=0
for p in "${probes[@]}"; do
    name="$(basename "$p")"
    src="$root/test/translation-probes/$p.lox"
    if [ ! -f "$src" ]; then
        echo "  FAIL  $name (no such probe: $src)"
        failures=$((failures + 1))
        continue
    fi

    if ! "$qbe_emit_program" "$src" >"$name.ssa" 2>"$name.descs"; then
        echo "  FAIL  $name (qbe_emit_program)"
        sed 's/^/          /' "$name.descs"
        failures=$((failures + 1))
        continue
    fi

    # Generate the per-probe harness: one extern decl + one RtFunctionDesc
    # entry per "id arity hash symbol" line qbe_emit_program wrote to
    # stderr.
    harness_cpp="${name}_harness.cpp"
    {
        echo '#include "backend/rt_abi.h"'
        echo '#include "backend/rt_capi.h"'
        echo '#include <cstdio>'
        echo '#include <fstream>'
        echo '#include <sstream>'
        echo '#include <string>'
        echo
        while read -r id arity hash symbol; do
            echo "extern \"C\" int ${symbol}(Runtime*, Value*);"
        done <"$name.descs"
        echo
        echo 'int main(int argc, char** argv) {'
        echo '    if (argc < 2) { std::fprintf(stderr, "usage: harness program.lox\n"); return 64; }'
        echo '    std::ifstream file(argv[1]);'
        echo '    if (!file) { std::fprintf(stderr, "harness: cannot read %s\n", argv[1]); return 74; }'
        echo '    std::stringstream ss; ss << file.rdbuf();'
        echo '    std::string source = ss.str();'
        echo '    RtFunctionDesc descs[] = {'
        while read -r id arity hash symbol; do
            echo "        {\"${id}\", ${arity}, ${hash}ULL, reinterpret_cast<void*>(&${symbol})},"
        done <"$name.descs"
        echo '    };'
        echo '    Runtime* rt = rt_startup(source.c_str(), descs, sizeof(descs) / sizeof(descs[0]), true);'
        echo '    if (rt == nullptr) { return 65; }'
        echo '    int status = rt_call(rt, 0);'
        echo '    rt_shutdown(rt);'
        echo '    return status == 0 ? 0 : 70;'
        echo '}'
    } >"$harness_cpp"

    if ! qbe -o "$name.s" "$name.ssa" 2>"$name.qbe_err"; then
        echo "  FAIL  $name (qbe)"
        sed 's/^/          /' "$name.qbe_err"
        failures=$((failures + 1))
        continue
    fi

    if ! c++ -std=c++17 -I "$root/src" "$harness_cpp" "$name.s" "$libloxrt" \
             -lstdc++ -lm -o "${name}_bin" 2>"$name.cc_err"; then
        echo "  FAIL  $name (cc)"
        sed 's/^/          /' "$name.cc_err"
        failures=$((failures + 1))
        continue
    fi

    qbe_out="$(./"${name}_bin" "$src" 2>"$name.qbe_stderr")" || qbe_status=$?
    qbe_status="${qbe_status:-0}"
    native_out="$("$loxpp_bin" "$src" 2>"$name.native_stderr")" || native_status=$?
    native_status="${native_status:-0}"

    if [ "$qbe_out" = "$native_out" ] && [ "$qbe_status" = "$native_status" ]; then
        diagnostic_ok=1
        if requires_overflow_diagnostic "$name"; then
            if ! grep -q "Stack overflow\." "$name.qbe_stderr" ||
               ! grep -q "Stack overflow\." "$name.native_stderr"; then
                diagnostic_ok=0
            fi
        fi
        if [ "$diagnostic_ok" -eq 1 ]; then
            printf '  ok    %s\n' "$name"
        else
            printf '  FAIL  %s (stdout+exit matched, but the "Stack overflow." diagnostic did not)\n' "$name"
            printf '          qbe    stderr: %s\n' "$(head -1 "$name.qbe_stderr")"
            printf '          native stderr: %s\n' "$(head -1 "$name.native_stderr")"
            failures=$((failures + 1))
        fi
    else
        printf '  FAIL  %s\n' "$name"
        printf '          qbe    (exit %s): %s\n' "$qbe_status" "$qbe_out"
        printf '          native (exit %s): %s\n' "$native_status" "$native_out"
        failures=$((failures + 1))
    fi
    unset qbe_status native_status
done

echo
if [ "$failures" -ne 0 ]; then
    echo "$failures S4 checkpoint check(s) failed."
    exit 1
fi
echo "S4 checkpoint passed."
