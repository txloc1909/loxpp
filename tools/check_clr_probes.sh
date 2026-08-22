#!/usr/bin/env bash
#
# tools/loxpp_clr.sh's stdout on each probe must be identical to build/loxpp's
# own stdout, byte for byte. Compares with diff, not by eye. The CLR twin of
# tools/check_jvm_probes.sh, scoped today to the straight-line opcode set
# (CONSTANT, NIL/TRUE/FALSE, arithmetic/comparison, NEGATE, NOT, PRINT, POP,
# GET_LOCAL, SET_LOCAL, DEFINE_GLOBAL, GET_GLOBAL, SET_GLOBAL), control flow
# (JUMP, JUMP_IF_FALSE, LOOP), functions and calls (CALL, RETURN's dual
# role), and closures and upvalues (CLOSURE with a captured cell,
# GET_UPVALUE, SET_UPVALUE, CLOSE_UPVALUE, plus BUILD_LIST, BUILD_MAP,
# GET_INDEX, and SET_INDEX, pulled forward because the closure probes need
# a list to hold the closures under test) — later CLR backend work grows
# this list the same way check_jvm_probes.sh grew as the JVM backend gained
# opcodes.
#
# error_probes hold the opposite shape: both sides must FAIL, with matching
# stdout. They check that an error stays an error on the CLR backend too, not
# only that a success stays a success.
#
# examples holds whole example programs from examples/, each one exercising
# more of the accumulated CLR surface at once than a single probe does.
#

# Every probe runs even after an earlier one fails: a failing CLR run (an
# ilasm assembly error, say) is caught and recorded as this probe's own
# failure, not left to `set -e` at top level, which would otherwise stop the
# loop at the first failure and hide every later probe.
#
# Requires build/loxpp (LOXPP_CLR_BACKEND, default ON) and
# runtime/clr/LoxRuntime.dll (tools/build_lox_rt_clr.sh) already built.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
native_bin="${LOXPP_BIN:-$root/build/loxpp}"
rt_dll="${LOX_RT_CLR_DLL:-$root/runtime/clr/LoxRuntime.dll}"

probes=(
    "notes/translation-probes/01_assign_local.lox"
    "notes/translation-probes/15_nested_arith.lox"
    "notes/translation-probes/18_peek_of_named_local.lox"
    "notes/translation-probes/19_peek_of_named_local_global.lox"
    "notes/translation-probes/20_float_imprecise_constant.lox"
    "notes/translation-probes/21_exponent_constant.lox"
    "notes/translation-probes/30_bool_compare_and_string_literal.lox"
    # Control flow (JUMP, JUMP_IF_FALSE, LOOP): if/else, and/or, while, for,
    # and the two short-circuit merge shapes that broke the JVM emitter
    # after it first looked correct (the merge POP staying real, and the
    # peek-of-a-materialized-condition reload).
    "notes/translation-probes/02_if_else.lox"
    "notes/translation-probes/03_and_or.lox"
    "notes/translation-probes/04_while.lox"
    "notes/translation-probes/05_for.lox"
    "notes/translation-probes/22_and_or_assignment_statement.lox"
    "notes/translation-probes/23_and_or_local_initializer.lox"
    # Functions and calls: CALL's own argument-array reshape, a zero-upvalue
    # CLOSURE building a generated class, and RETURN's function-role value
    # return.
    "notes/translation-probes/08_call.lox"
    # CALL of a LoxNative, not only a LoxClosure — LoxOps.Call reaches a
    # native through its own argument-count check, a different path than a
    # closure's, and no earlier probe in this list exercises it.
    "notes/translation-probes/29_os_access.lox"
    # Closures and upvalues (the bug gate): V1_fresh_cell is the standing
    # counter-example a naive one-cell-per-local-at-function-entry design
    # gets WRONG (2 2 2) while V3_loopvar's shared cell (3 3 3) looks
    # right — see ensureCapturedCell's own note. V2_shared and
    # 06_shared_upvalue prove two closures over the same live incarnation
    # share one cell; V4 proves the shared cell is really shared, by
    # mutating it through one closure and reading it through another (06
    # alone proves only that the module loads). V5/V6 prove a local `fun`
    # that captures itself seeds its own cell before its first read.
    # BUILD_LIST, BUILD_MAP, GET_INDEX, and SET_INDEX are pulled forward
    # here too: V1 and V3 each build a list of the closures under test and
    # read it back by index, and 12_list_map_index needs a map as well as
    # a list to run at all — none of these probes can even compile without
    # aggregate and index support.
    "notes/translation-probes/06_shared_upvalue.lox"
    "notes/translation-probes/V1_fresh_cell.lox"
    "notes/translation-probes/V2_shared.lox"
    "notes/translation-probes/V3_loopvar.lox"
    "notes/translation-probes/V4_mutate_through_upvalue.lox"
    "notes/translation-probes/V5_self_recursive_closure.lox"
    "notes/translation-probes/V6_self_recursive_closure_in_loop.lox"
    "notes/translation-probes/12_list_map_index.lox"
)

# Probes that must FAIL on both sides: a global function called before its
# own `fun` declaration has run is a late-bound-global error, not a silent
# success. Each entry needs a non-zero exit from build/loxpp AND from
# tools/loxpp_clr.sh, with matching stdout — empty for 24 and 31, since
# neither side prints anything before the error.
error_probes=(
    "notes/translation-probes/24_call_before_closure.lox"
    # Native's own frame-count ceiling (src/vm.h FRAMES_MAX): a backend
    # whose calling convention recurses its own host call stack has no
    # such ceiling unless it builds one, and a wrong build here is a
    # silent success on stdout otherwise. Lives in clr-only/, not directly
    # in translation-probes/, so tools/diff_runtimes.py's whole-directory
    # walk against the JVM backend does not see it (see the probe file's
    # own header comment for why).
    "notes/translation-probes/clr-only/31_deep_recursion.lox"
)

# Whole example programs, not single-opcode probes: each one exercises more
# of the accumulated CLR surface at once than any single probe does (fibonacci
# and hanoi both recurse through CALL). check_examples.py already gates these
# against the native binary and the JVM backend; this group is the CLR twin
# of that gate, so a runnable example cannot regress on the CLR backend
# unnoticed. A later node adds its own newly runnable examples here.
examples=(
    "examples/collatz.lox"
    "examples/fibonacci.lox"
    "examples/fizzbuzz.lox"
    "examples/guessing_game.lox"
    "examples/hanoi.lox"
    "examples/leap_year.lox"
    # This node's own newly runnable examples: each one exercises
    # BUILD_LIST, GET_INDEX, or SET_INDEX (a list or string literal,
    # indexed or index-assigned) that a prior node's emitter would reject
    # outright. Confirmed by running the whole corpus through
    # tools/loxpp_clr.sh: these nine, with the six above, are every
    # example that runs to completion on the CLR backend and matches
    # native byte for byte. Every other example in the corpus still needs
    # an opcode this pass does not yet lower (INVOKE for a list/map
    # method, IN, BUILD_MAP through a Map method, classes, or match) and
    # exits with an error on this backend today.
    "examples/digital_root.lox"
    "examples/gcd_lcm.lox"
    "examples/to_binary.lox"
    "examples/palindrome.lox"
    "examples/luhn.lox"
    "examples/clock_arithmetic.lox"
    "examples/anagram.lox"
    "examples/caesar.lox"
    "examples/linear_regression.lox"
)

if [ ! -x "$native_bin" ]; then
    echo "check_clr_probes.sh: no loxpp binary at $native_bin" >&2
    exit 1
fi

native_out="$(mktemp)"
native_err="$(mktemp)"
clr_out="$(mktemp)"
clr_err="$(mktemp)"
trap 'rm -f "$native_out" "$native_err" "$clr_out" "$clr_err"' EXIT

failed_probes=()
for probe in "${probes[@]}"; do
    if ! "$native_bin" "$root/$probe" >"$native_out" 2>"$native_err"; then
        echo "check_clr_probes.sh: FAIL $probe (native run failed)" >&2
        cat "$native_err" >&2
        failed_probes+=("$probe")
        continue
    fi
    if ! "$root/tools/loxpp_clr.sh" "$root/$probe" >"$clr_out" 2>"$clr_err"; then
        echo "check_clr_probes.sh: FAIL $probe (CLR run failed)" >&2
        cat "$clr_err" >&2
        failed_probes+=("$probe")
        continue
    fi
    if diff -u "$native_out" "$clr_out"; then
        echo "check_clr_probes.sh: OK $probe"
    else
        echo "check_clr_probes.sh: FAIL $probe (stdout mismatch)" >&2
        failed_probes+=("$probe")
    fi
done

for probe in "${error_probes[@]}"; do
    "$native_bin" "$root/$probe" >"$native_out" 2>"$native_err"
    native_status=$?
    if [ "$native_status" -eq 0 ]; then
        echo "check_clr_probes.sh: FAIL $probe (native run did not fail)" >&2
        failed_probes+=("$probe")
        continue
    fi

    # Emission and execution are two separate facts. A probe must fail at RUN
    # time, the same place the native side fails, not at emit time (an
    # unimplemented opcode, say). tools/loxpp_clr.sh exits non-zero either
    # way, so checking only its exit code would let an emit-time abort
    # satisfy this loop by accident.
    il_dir="$(mktemp -d)"
    if ! "$native_bin" --target clr --out-dir "$il_dir" "$root/$probe" \
        >/dev/null 2>"$clr_err"; then
        echo "check_clr_probes.sh: FAIL $probe (CLR emit failed, not a runtime error)" >&2
        cat "$clr_err" >&2
        rm -rf "$il_dir"
        failed_probes+=("$probe")
        continue
    fi
    "$root/tools/clr_run.sh" "$il_dir" "$rt_dll" LoxMain \
        >"$clr_out" 2>"$clr_err"
    clr_status=$?
    rm -rf "$il_dir"
    if [ "$clr_status" -eq 0 ]; then
        echo "check_clr_probes.sh: FAIL $probe (CLR run did not fail)" >&2
        failed_probes+=("$probe")
        continue
    fi
    # A non-zero exit is not proof of a Lox error. CoreCLR gives the same
    # non-zero exit for a host fault it hits before the Lox program itself
    # can raise anything: a bad rt_dll path, or invalid IL that throws
    # InvalidProgramException at JIT time (this image has no IL verifier
    # to catch that earlier — see brief.md section 5). Only a stderr line
    # naming Lox.LoxError proves the failure is the Lox-level error this
    # probe exists to check for.
    if ! grep -q "Lox.LoxError" "$clr_err"; then
        echo "check_clr_probes.sh: FAIL $probe (CLR run failed, but not with a Lox.LoxError)" >&2
        cat "$clr_err" >&2
        failed_probes+=("$probe")
        continue
    fi
    if diff -u "$native_out" "$clr_out"; then
        echo "check_clr_probes.sh: OK $probe (both failed, stdout matches)"
    else
        echo "check_clr_probes.sh: FAIL $probe (stdout mismatch)" >&2
        failed_probes+=("$probe")
    fi
done

for example in "${examples[@]}"; do
    # examples/<name>.input holds stdin for a program that calls input(),
    # exactly as check_examples.py reads it. Most examples have none.
    input_file="$root/${example%.lox}.input"
    if [ -f "$input_file" ]; then
        native_ok=1; "$native_bin" "$root/$example" <"$input_file" \
            >"$native_out" 2>"$native_err" || native_ok=0
    else
        native_ok=1; "$native_bin" "$root/$example" \
            >"$native_out" 2>"$native_err" || native_ok=0
    fi
    if [ "$native_ok" -eq 0 ]; then
        echo "check_clr_probes.sh: FAIL $example (native run failed)" >&2
        cat "$native_err" >&2
        failed_probes+=("$example")
        continue
    fi
    if [ -f "$input_file" ]; then
        clr_ok=1; "$root/tools/loxpp_clr.sh" "$root/$example" <"$input_file" \
            >"$clr_out" 2>"$clr_err" || clr_ok=0
    else
        clr_ok=1; "$root/tools/loxpp_clr.sh" "$root/$example" \
            >"$clr_out" 2>"$clr_err" || clr_ok=0
    fi
    if [ "$clr_ok" -eq 0 ]; then
        echo "check_clr_probes.sh: FAIL $example (CLR run failed)" >&2
        cat "$clr_err" >&2
        failed_probes+=("$example")
        continue
    fi
    if diff -u "$native_out" "$clr_out"; then
        echo "check_clr_probes.sh: OK $example"
    else
        echo "check_clr_probes.sh: FAIL $example (stdout mismatch)" >&2
        failed_probes+=("$example")
    fi
done

if [ "${#failed_probes[@]}" -ne 0 ]; then
    echo "check_clr_probes.sh: ${#failed_probes[@]} probe(s) failed:" >&2
    for probe in "${failed_probes[@]}"; do
        echo "  $probe" >&2
    done
    exit 1
fi

echo "check_clr_probes.sh: all $((${#probes[@]} + ${#error_probes[@]} + ${#examples[@]})) probes OK"
