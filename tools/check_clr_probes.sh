#!/usr/bin/env bash
#
# tools/loxpp_clr.sh's stdout on each probe must be identical to build/loxpp's
# own stdout, byte for byte. Compares with diff, not by eye. The CLR twin of
# tools/check_jvm_probes.sh: every entry in the probes array below runs on
# both backends, and any stdout difference fails the gate naming that probe.
# The group comments in the probes array name the surface each block adds;
# neither this header nor any group comment below is itself a completeness
# claim — the corpus sweep, further down, is the one check that enforces
# completeness against examples/, and it fails by name when it finds a gap.
#
# error_probes hold the opposite shape: both sides must FAIL, with matching
# stdout. They check that an error stays an error on the CLR backend too, not
# only that a success stays a success.
#
# examples holds whole example programs from examples/, each one exercising
# more of the accumulated CLR surface at once than a single probe does.
#
# The corpus sweep, below the examples loop, checks that the examples group
# stays complete. It runs every examples/*.lox file NOT already in the
# examples array and NOT in tools/clr_excluded_examples.txt, the same way
# the loop above runs the ones that are, and fails the script naming the
# file whenever one of them runs to completion on both backends: a
# byte-identical match belongs in the examples array, and any other
# same-outcome difference (a map-order permutation or a real divergence)
# belongs in tools/clr_excluded_examples.txt or is a live defect — either
# way, the sweep reports it rather than passing over it in silence. This
# turns "the group holds every example the CLR backend can run" from a
# sentence a person writes by hand into something the script checks on
# every run.
#
# tools/clr_excluded_examples.txt names the map-order permutations the CLR
# gate accepts (spec/03-types.md leaves map iteration order unspecified),
# the CLR twin of tools/jvm_excluded_examples.txt. Every run of this
# script re-checks each entry with tools/diff_runtimes.py --only-excluded:
# an entry whose CLR stdout stops being a permutation of native stdout
# fails the gate as a real divergence, not a silently-forgiven exclusion.
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

# native_bin's own stringifyObj (src/object.cpp) recurses once per level of
# list/map nesting with no depth guard, the same shape as this script's
# 39_deep_nested_stringify.lox probe checks on the CLR side (see that
# file's own header). This probe's native run needs a process stack big
# enough for that depth; a host whose default soft limit is already lower
# than the probe needs would otherwise fail that probe's NATIVE run, which
# is not a CLR defect.
#
# The raise lives in this one function, in a subshell, so it reaches only
# native_bin's own process — never this script's own shell, and never any
# CLR child process this script goes on to run. A raise at this script's
# own top level would leak into every later `dotnet` child too (`clr_run.sh`
# execs it), silently giving the CLR side a bigger main-thread stack no
# matter how it sizes its own thread, which would hide a regression that
# stops routing a CLR run through LoxHost's own larger thread
# (runtime/clr/host/LoxHost.cs). A host whose HARD limit is already below
# the floor still gets its soft limit raised, but only up to that hard
# ceiling, never past it; if the ceiling itself is smaller than the depth
# this probe needs, the native run still fails, for a reason unrelated to
# the CLR backend.
#
# This is a raise, never a lowering. `ulimit -Ss` prints "unlimited" for an
# already-unbounded soft limit, and an unconditional `ulimit -Ss 65536`
# would silently REPLACE "unlimited" with the finite floor — a lowering,
# not a raise. So this function checks the current soft limit first, caps
# the target at the current HARD limit when that limit is finite, and only
# raises the soft limit when the resulting target is above it.
# tools/diff_runtimes.py's _raise_native_stack_limit is this function's
# Python twin and follows the identical rule, against the identical
# 64 MiB floor.
run_native() {
    (
        current_soft="$(ulimit -Ss)"
        if [ "$current_soft" != "unlimited" ]; then
            current_hard="$(ulimit -Hs)"
            if [ "$current_hard" = "unlimited" ]; then
                ceiling=65536
            else
                ceiling="$current_hard"
            fi
            if [ "$ceiling" -lt 65536 ]; then
                target="$ceiling"
            else
                target=65536
            fi
            if [ "$target" -gt "$current_soft" ]; then
                ulimit -Ss "$target" 2>/dev/null || true
            fi
        fi
        "$native_bin" "$@"
    )
}

probes=(
    "notes/translation-probes/01_assign_local.lox"
    "notes/translation-probes/15_nested_arith.lox"
    "notes/translation-probes/18_peek_of_named_local.lox"
    "notes/translation-probes/19_peek_of_named_local_global.lox"
    "notes/translation-probes/20_float_imprecise_constant.lox"
    "notes/translation-probes/21_exponent_constant.lox"
    "notes/translation-probes/30_bool_compare_and_string_literal.lox"
    "notes/translation-probes/32_string_nul.lox"
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
    # Classes, methods, and super: `this` = slot 0, `init` returns `this`,
    # SET_PROPERTY/DEFINE_METHOD leave a value (P2), and `super` is compiled
    # as an upvalue capture of the superclass (P4) — GET_SUPER reads it as
    # a value, not only through a call.
    "notes/translation-probes/09_class.lox"
    "notes/translation-probes/10_super.lox"
    "notes/translation-probes/17_super_value.lox"
    # INVOKE's field-shadow arm called on a bound built-in method value (a
    # map or file method read through GET_PROPERTY, then stored in an
    # instance field and called through the field name), not only a
    # closure or an unbound native. Lives in clr-only/ (see the probe
    # file's own header comment for why); named here directly.
    "notes/translation-probes/clr-only/37_invoke_field_bound_native_method.lox"
    # Equal on a map method value read through GET_PROPERTY: a fresh
    # object on every read, so `==` gives false whether the two reads
    # share a receiver or not. The file method value follows the same
    # rule through the same Equal arm; runtime/clr/test/FileTest.cs pins
    # that half at the runtime level.
    "notes/translation-probes/38_bound_native_method_identity.lox"
    # notes/translation-probes/clr-only/39_deep_nested_stringify.lox is not
    # in this array: it needs two runs at two different CLR thread stack
    # sizes, not one, so it has its own paired check below this loop.
    # The consumed-match case: a match expression's result reaching PRINT,
    # DEFINE_GLOBAL, or SET_GLOBAL with nothing in between to re-expose it
    # as a genuine evaluation-stack value first.
    "notes/translation-probes/34_match_consumed_result.lox"
    # Aggregates, slices, membership, and iterators: the for-in protocol
    # (GET_ITER/ITER_HAS_NEXT/ITER_NEXT — GET_ITER's own hazard is that it
    # runs on an already-empty evaluation stack, see emitGetIter's own
    # note), SLICE and IN over a List/String/Map, and IS_SEQ (a match
    # sequence pattern's own type check).
    "notes/translation-probes/11_for_in.lox"
    "notes/translation-probes/16_slice_in.lox"
    "notes/translation-probes/25_seq_map_string_coverage.lox"
    # Match/enum dispatch: a dense match over enum variants (GET_TAG fused
    # with JUMP_TABLE, a CIL `switch`), an enum-constructor CONSTANT and
    # CALL, an enum payload read through GET_INDEX, and
    # normalizeFoldedOperands's own repair for a folded match operand
    # reaching a genuine sibling operand of the same consumer.
    "notes/translation-probes/13_enum_match.lox"
    "notes/translation-probes/14_enum_payload.lox"
    "notes/translation-probes/28_folded_match_operand_family.lox"
    # A fold deficit of two or more: every operand the multi-slot repair
    # reloads is itself folded, with no genuine value between them on the
    # real CIL evaluation stack, over ADD/CALL/BUILD_LIST/BUILD_MAP, plus
    # two of the folded slots also being captured-closure slots. No JVM
    # twin: the JVM side's own repair refuses a deficit above one, so this
    # file lives in clr-only/ and is named here directly.
    "notes/translation-probes/clr-only/35_folded_match_deficit_two_plus.lox"
    # The shared scratch area's own WIDTH, not the multi-slot load order
    # probe 35 already covers: a SLICE or SET_INDEX consumer whose fold
    # deficit leaves two or more genuine operands live, in a chunk that
    # builds no CALL/INVOKE/SUPER_INVOKE/BUILD_LIST/BUILD_MAP of its own to
    # size the area by coincidence, in both a script chunk and a function
    # chunk. No JVM twin: the JVM backend reorders with `swap` instead of a
    # scratch area of a computed width, so this file lives in clr-only/ and
    # is named here directly.
    "notes/translation-probes/clr-only/36_folded_operand_spill_sizing.lox"
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
    # A match whose arms are all class patterns raises a real, reachable
    # MATCH_ERROR when no arm matches — both sides print "before" then fail.
    "notes/translation-probes/33_class_pattern_match_error.lox"
    # A dense, table-eligible match over enum A's own tags, given a subject
    # of an unrelated enum B: the literal JUMP_TABLE default, not the sparse
    # compare-and-branch form, raises MATCH_ERROR — both sides print
    # "before" then fail.
    "notes/translation-probes/27_jump_table_default_cross_enum.lox"
    # A dense enum match that dispatches correctly, then a second match
    # whose guard defeats every arm despite naming every constructor once
    # (exhaustive by name, accepting nothing at run time) — both sides
    # print the first match's own arm value, then fail on the second.
    "notes/translation-probes/26_enum_match_dispatch_and_error.lox"
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
    # Aggregate literals and indexing: each of these nine needs BUILD_LIST,
    # GET_INDEX, or SET_INDEX to run — a list or string literal, indexed or
    # index-assigned. Whether this group is complete is what the corpus
    # sweep below checks, not this comment.
    "examples/digital_root.lox"
    "examples/gcd_lcm.lox"
    "examples/to_binary.lox"
    "examples/palindrome.lox"
    "examples/luhn.lox"
    "examples/clock_arithmetic.lox"
    "examples/anagram.lox"
    "examples/caesar.lox"
    "examples/linear_regression.lox"
    # Classes, methods, and super: class_dispatch.lox is the first program
    # whose `return match {...}` reaches the RETURN-of-a-named-local case
    # end to end (bytecode-translation-problems.md); shapes.lox
    # additionally exercises inheritance, dynamic dispatch through
    # INVOKE, and `math.pi` through GET_PROPERTY on a
    # native-function-bearing instance.
    "examples/class_dispatch.lox"
    "examples/shapes.lox"
    "examples/ast_eval.lox"
    "examples/flatten.lox"
    "examples/higher_order.lox"
    "examples/multi_return.lox"
    "examples/quiz.lox"
    "examples/stack_queue.lox"
    # Folded match-result exposure through PRINT and DEFINE_GLOBAL
    # (emitPrint's own note): both print a match expression's result
    # directly, with nothing in between to re-expose it as a genuine
    # evaluation-stack value first.
    "examples/match_http_status.lox"
    "examples/match_state_machine.lox"
    "examples/csv_reader.lox"
    "examples/data_pipeline.lox"
    "examples/graph_bfs_dfs.lox"
    "examples/histogram.lox"
    "examples/line_sorter.lox"
    "examples/log_writer.lox"
    "examples/math_demo.lox"
    "examples/memo_fib.lox"
    "examples/newton_sqrt.lox"
    "examples/polar.lox"
    "examples/remove.lox"
    "examples/sieve.lox"
    "examples/stats.lox"
    "examples/wc.lox"
    # Membership, slicing, and sequence patterns: each of these nine needs
    # the `in` operator, SLICE syntax (`xs[a:b]`), or a match sequence
    # pattern (IS_SEQ) to run.
    "examples/config_parser.lox"
    "examples/grep_lite.lox"
    "examples/match_dispatch.lox"
    "examples/merge_sort.lox"
    "examples/run_length.lox"
    "examples/scanner.lox"
    "examples/sequences.lox"
    "examples/sorting.lox"
    "examples/string_list_pattern_demo.lox"
    "examples/huffman.lox"
    # Match/enum dispatch: each of these needs GET_TAG/JUMP_TABLE, an
    # enum-constructor CONSTANT and CALL, or an or-pattern/@-binding over an
    # enum to run.
    "examples/enum_match.lox"
    "examples/enum_result.lox"
    "examples/enum_tree.lox"
    "examples/at_binding_demo.lox"
    "examples/bench_jump_table.lox"
    "examples/or_pattern_demo.lox"
    "examples/parser.lox"
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
    if ! run_native "$root/$probe" >"$native_out" 2>"$native_err"; then
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

# This is the one place that owns the relation between the CLR thread's
# stack size and notes/translation-probes/clr-only/39_deep_nested_stringify.lox's
# own ability to fail: two runs of the same probe, at two different sizes,
# checked together, so neither row can go green for the wrong reason.
#
#   MUST-PASS — LOX_CLR_STACK_BYTES explicitly unset for this run, so
#   ambient state in the calling shell cannot change which size
#   tools/clr_run.sh picks; the run must match native's stdout byte for
#   byte, and native's stdout must not be empty. Unsetting rather than
#   restating tools/clr_run.sh's own default byte count keeps that number
#   in the one file that already owns it.
#
#   MUST-FAIL — LOX_CLR_STACK_BYTES pinned to 8388608, the measured 8 MiB
#   thread size at which LoxOps.Stringify's own unbounded recursion
#   overflows this probe's depth (see the probe file's own header). This
#   run must either exit non-zero or print something other than native's
#   stdout. If it instead exits zero with output matching native, running
#   the CLR side on a small thread is no longer what this probe catches,
#   so this check fails and names the probe disarmed, rather than let the
#   MUST-PASS row above stand in for a proof it does not give.
stack_limit_probe="notes/translation-probes/clr-only/39_deep_nested_stringify.lox"
stack_limit_overflow_bytes=8388608
stack_limit_checks=2

if ! run_native "$root/$stack_limit_probe" >"$native_out" 2>"$native_err"; then
    echo "check_clr_probes.sh: FAIL $stack_limit_probe (native run failed)" >&2
    cat "$native_err" >&2
    failed_probes+=("$stack_limit_probe (native)")
elif [ ! -s "$native_out" ]; then
    echo "check_clr_probes.sh: FAIL $stack_limit_probe (native run gave empty stdout, so a match would prove nothing)" >&2
    failed_probes+=("$stack_limit_probe (native empty)")
else
    must_pass_ok=1
    env -u LOX_CLR_STACK_BYTES "$root/tools/loxpp_clr.sh" "$root/$stack_limit_probe" \
        >"$clr_out" 2>"$clr_err" || must_pass_ok=0
    if [ "$must_pass_ok" -eq 0 ] || ! diff -q "$native_out" "$clr_out" >/dev/null; then
        echo "check_clr_probes.sh: FAIL $stack_limit_probe (MUST-PASS: default-size CLR thread does not match native)" >&2
        cat "$clr_err" >&2
        failed_probes+=("$stack_limit_probe (MUST-PASS)")
    else
        echo "check_clr_probes.sh: OK $stack_limit_probe (MUST-PASS: default-size CLR thread matches native)"
    fi

    must_fail_ok=1
    LOX_CLR_STACK_BYTES="$stack_limit_overflow_bytes" \
        "$root/tools/loxpp_clr.sh" "$root/$stack_limit_probe" \
        >"$clr_out" 2>"$clr_err" || must_fail_ok=0
    if [ "$must_fail_ok" -eq 1 ] && diff -q "$native_out" "$clr_out" >/dev/null; then
        echo "check_clr_probes.sh: FAIL $stack_limit_probe (MUST-FAIL: an $stack_limit_overflow_bytes-byte CLR thread matched native — probe disarmed)" >&2
        failed_probes+=("$stack_limit_probe (MUST-FAIL, disarmed)")
    else
        echo "check_clr_probes.sh: OK $stack_limit_probe (MUST-FAIL: an $stack_limit_overflow_bytes-byte CLR thread does not reproduce native's output, so the probe can still catch the regression)"
    fi
fi

for probe in "${error_probes[@]}"; do
    run_native "$root/$probe" >"$native_out" 2>"$native_err"
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
    # to catch that earlier). Only a stderr line naming Lox.LoxError proves
    # the failure is the Lox-level error this probe exists to check for.
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
        native_ok=1; run_native "$root/$example" <"$input_file" \
            >"$native_out" 2>"$native_err" || native_ok=0
    else
        native_ok=1; run_native "$root/$example" \
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

# --- CLR permutation guard -------------------------------------------------
# Re-proves, on every run, that each tools/clr_excluded_examples.txt entry's
# CLR stdout is still a permutation of native stdout, not a content change
# that the exclusion is silently hiding. tools/diff_runtimes.py already
# does exactly this for tools/jvm_excluded_examples.txt; --only-excluded
# runs it over exactly the excluded programs, resolved under examples/.
excluded_list="$root/tools/clr_excluded_examples.txt"
if ! python3 "$root/tools/diff_runtimes.py" "$native_bin" \
        "$root/tools/loxpp_clr.sh" --exclude "$excluded_list" \
        --only-excluded "$root/examples"; then
    echo "check_clr_probes.sh: FAIL CLR permutation guard (tools/clr_excluded_examples.txt)" >&2
    failed_probes+=("clr_excluded_examples_permutation_guard")
else
    echo "check_clr_probes.sh: CLR permutation guard OK, every exclusion is still a map-order permutation"
fi

# --- corpus sweep --------------------------------------------------------
# Enforces the completeness claim the comment above no longer makes in
# prose. Every examples/*.lox file NOT already in the examples array, and
# NOT in tools/clr_excluded_examples.txt (covered by the permutation guard
# above instead), runs on both backends here, with the same .input rule as
# the loop above. Three outcomes when a program runs to completion (exit 0)
# on BOTH sides:
#
#   - byte-identical stdout: belongs in the examples array and is missing
#     from it.
#   - a map-order permutation (sorted lines match, unsorted lines do not):
#     belongs in tools/clr_excluded_examples.txt and is missing from it.
#   - any other difference: a real divergence — a live emitter defect the
#     sweep just found, not a gate to add an example to.
#
# All three are reported by name, not silently passed over. A hand-written
# sentence claiming completeness cannot make this true; the next opcode
# this backend gains would make it false without anyone noticing.
in_examples_group() {
    local candidate="$1"
    for known in "${examples[@]}"; do
        if [ "$known" = "$candidate" ]; then
            return 0
        fi
    done
    return 1
}

in_excluded_list() {
    local candidate_name="$1"
    awk '!/^[[:space:]]*#/ && NF {print $1}' "$excluded_list" | grep -qxF "$candidate_name"
}

is_permutation() {
    diff <(sort "$1") <(sort "$2") >/dev/null
}

sweep_missing=()
sweep_permutation_missing=()
sweep_diverging=()
for example_path in "$root"/examples/*.lox; do
    example="examples/$(basename "$example_path")"
    if in_examples_group "$example"; then
        continue
    fi
    if in_excluded_list "$(basename "$example_path")"; then
        continue
    fi

    input_file="$root/${example%.lox}.input"
    if [ -f "$input_file" ]; then
        native_ok=1; run_native "$root/$example" <"$input_file" \
            >"$native_out" 2>"$native_err" || native_ok=0
    else
        native_ok=1; run_native "$root/$example" \
            >"$native_out" 2>"$native_err" || native_ok=0
    fi
    if [ "$native_ok" -eq 0 ]; then
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
        continue
    fi

    if diff -q "$native_out" "$clr_out" >/dev/null; then
        sweep_missing+=("$example")
    elif is_permutation "$native_out" "$clr_out"; then
        sweep_permutation_missing+=("$example")
    else
        sweep_diverging+=("$example")
    fi
done

sweep_ok=1
if [ "${#sweep_missing[@]}" -ne 0 ]; then
    sweep_ok=0
    echo "check_clr_probes.sh: ${#sweep_missing[@]} example(s) run to completion and match native byte for byte, but are missing from the examples group:" >&2
    for example in "${sweep_missing[@]}"; do
        echo "  $example" >&2
    done
    failed_probes+=("${sweep_missing[@]}")
fi
if [ "${#sweep_permutation_missing[@]}" -ne 0 ]; then
    sweep_ok=0
    echo "check_clr_probes.sh: ${#sweep_permutation_missing[@]} example(s) run to completion on both sides as a map-order permutation of each other, but are missing from tools/clr_excluded_examples.txt:" >&2
    for example in "${sweep_permutation_missing[@]}"; do
        echo "  $example" >&2
    done
    failed_probes+=("${sweep_permutation_missing[@]}")
fi
if [ "${#sweep_diverging[@]}" -ne 0 ]; then
    sweep_ok=0
    echo "check_clr_probes.sh: ${#sweep_diverging[@]} example(s) run to completion on both sides with DIFFERENT stdout that is NOT a map-order permutation — a real divergence, not a missing gate entry:" >&2
    for example in "${sweep_diverging[@]}"; do
        echo "  $example" >&2
    done
    failed_probes+=("${sweep_diverging[@]}")
fi
if [ "$sweep_ok" -eq 1 ]; then
    echo "check_clr_probes.sh: corpus sweep OK, the examples group and the exclusion list are both complete"
fi

if [ "${#failed_probes[@]}" -ne 0 ]; then
    echo "check_clr_probes.sh: ${#failed_probes[@]} probe(s) failed:" >&2
    for probe in "${failed_probes[@]}"; do
        echo "  $probe" >&2
    done
    exit 1
fi

echo "check_clr_probes.sh: all $((${#probes[@]} + stack_limit_checks + ${#error_probes[@]} + ${#examples[@]})) probes OK, corpus sweep confirms the examples group is complete"
