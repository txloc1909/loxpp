#!/usr/bin/env bash
#
# Round-trips the S6 node's checkpoint (issue #459): try/catch, throw,
# defer, stack overflow. Covers the status protocol (Q2), catchable and
# fatal stack overflow (Q3), the unwind guard's own edge cases (a spoofed
# "kind" field, a throw on normal return, a throw that replaces an
# unrelated fault — all while a real StackOverflowError unwinds
# elsewhere), the two fatal fast paths that must skip pending defers
# entirely, a return leaking out of a still-open try, a throw ending a
# try body's own local bindings, and — the two probes every other one
# here leaves untested — a deferred call's side effect actually becoming
# observable on an ordinary successful return and on a caught throw.
# Every probe above the fatal/skip-path ones still passes with
# RUN_DEFERS wired to a no-op (none of them observes a deferred call
# that is expected to run and print), which is why the last two exist:
# each asserts non-empty stdout produced only by the deferred call
# itself. Same whole-program driver as S4/S5's own checkpoint scripts —
# see check_qbe_s5_rest_of_language.sh's own file comment for why.
#
# Build with the release preset first (AGENTS.md): the debug preset leaves
# LOXPP_DEBUG_PRINT_CODE/LOXPP_DEBUG_TRACE_EXECUTION on by default, so
# native's own stdout would carry a bytecode trace no QBE-compiled binary
# ever produces, and every probe would "diverge" even though nothing is
# wrong.
#
#   cmake --preset release && cmake --build build \
#       --target loxpp qbe_emit_program loxrt -j$(nproc)
#   tools/check_qbe_s6_errors.sh
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
loxpp_bin="${LOXPP_BIN:-$root/build/loxpp}"
qbe_emit_program="${QBE_EMIT_PROGRAM:-$root/build/qbe_emit_program}"
libloxrt="${LOX_RT_A:-$root/build/libloxrt.a}"

for bin in "$loxpp_bin" "$qbe_emit_program"; do
    if [ ! -x "$bin" ]; then
        echo "check_qbe_s6_errors: no executable at $bin (build it first)" >&2
        exit 1
    fi
done
if [ ! -f "$libloxrt" ]; then
    echo "check_qbe_s6_errors: $libloxrt not found (build it first)" >&2
    exit 1
fi

# Each entry is "name:relative/path/from/test/translation-probes" — most
# probes live directly in that directory, but three (stack-overflow's own
# reentrant twin, and the unwind-guard edge cases) sit one level below it,
# outside the directory tools/diff_runtimes.py's own CI walk covers, per
# translation-probes/README.md's own note on why.
probes=(
    "48_try_catch_instance_undefined_property_fatal:48_try_catch_instance_undefined_property_fatal"
    "49_try_catch_map_undefined_property_fatal:49_try_catch_map_undefined_property_fatal"
    "50_try_catch_non_instance_get_property_fatal:50_try_catch_non_instance_get_property_fatal"
    "51_try_catch_non_instance_set_property_fatal:51_try_catch_non_instance_set_property_fatal"
    "57_stack_overflow_catchable:57_stack_overflow_catchable"
    "58_stack_overflow_reentrant_fatal:58_stack_overflow_reentrant_fatal"
    "catch_overflow:catch_overflow"
    "defer_overflow_during_unwind:defer_overflow_during_unwind"
    "defer_overflow_kind_spoof:defer_overflow_kind_spoof"
    "defer_throw_on_normal_return_during_unwind:defer_throw_on_normal_return_during_unwind"
    "defer_replaces_unrelated_fault_during_unwind:defer_replaces_unrelated_fault_during_unwind"
    "57_defer_arity_fatal_fast_path:57_defer_arity_fatal_fast_path"
    "58_defer_overflow_fatal_fast_path:58_defer_overflow_fatal_fast_path"
    "59_return_out_of_try_leak:59_return_out_of_try_leak"
    "60_throw_ends_try_binding:60_throw_ends_try_binding"
    "defer_runs_on_normal_return:defer_runs_on_normal_return"
    "defer_runs_before_caught_throw_propagates:defer_runs_before_caught_throw_propagates"
)

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

echo "== S6 checkpoint: errors (try/catch, throw, defer, stack overflow) =="

failures=0
for entry in "${probes[@]}"; do
    name="${entry%%:*}"
    rel="${entry#*:}"
    src="$root/test/translation-probes/$rel.lox"
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

    # Same per-probe harness generation as S4/S5's own checkpoint scripts:
    # one extern decl + one RtFunctionDesc entry per "id arity hash symbol"
    # line qbe_emit_program wrote to stderr. rt_call's own return is
    # Runtime::OpResult (0=OK, 1/2/3 otherwise) — unaffected by S6's own
    # RtCompiledFn-internal kRtOk/kRtThrow/kRtFatal contract, which
    # Runtime::callCompiled already translates back into this same
    # OpResult convention (see rt_abi.h's own comment) — so this harness's
    # exit-code convention (0 on success, 70 otherwise, matching native's
    # own RUNTIME_ERROR exit code) is unchanged from S3-S5.
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

    if ! c++ -std=c++20 -I "$root/src" "$harness_cpp" "$name.s" "$libloxrt" \
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
        printf '  ok    %s\n' "$name"
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
    echo "$failures S6 checkpoint check(s) failed."
    exit 1
fi
echo "S6 checkpoint passed."
