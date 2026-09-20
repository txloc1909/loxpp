#!/usr/bin/env bash
#
# tools/loxpp_jvm.sh's stdout on each probe must be identical to build/
# loxpp's own stdout, byte for byte. Compares with diff, not by eye. New
# opcode support adds its own probes to this same list.
#
# error_probes hold the opposite shape: both sides must FAIL, with matching
# stdout. They check that an error stays an error on the JVM backend too,
# not only that a success stays a success.
#
# Every probe runs even after an earlier one fails: a failing JVM run (a
# verifier rejection, say) is caught and recorded as this probe's own
# failure, not left to `set -e` at top level, which would otherwise stop the
# loop at the first failure and hide every later probe.
#
# Requires build/loxpp (LOXPP_JVM_BACKEND, default ON) and
# runtime/jvm/lox-rt.jar (tools/build_lox_rt.sh) already built.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
native_bin="${LOXPP_BIN:-$root/build/loxpp}"
lox_rt_jar="${LOX_RT_JAR:-$root/runtime/jvm/lox-rt.jar}"

probes=(
    "test/translation-probes/01_assign_local.lox"
    "test/translation-probes/02_if_else.lox"
    "test/translation-probes/03_and_or.lox"
    "test/translation-probes/04_while.lox"
    "test/translation-probes/05_for.lox"
    "test/translation-probes/08_call.lox"
    "test/translation-probes/15_nested_arith.lox"
    "test/translation-probes/18_peek_of_named_local.lox"
    "test/translation-probes/19_peek_of_named_local_global.lox"
    "test/translation-probes/20_float_imprecise_constant.lox"
    "test/translation-probes/21_exponent_constant.lox"
    "test/translation-probes/22_and_or_assignment_statement.lox"
    "test/translation-probes/23_and_or_local_initializer.lox"
    "test/translation-probes/32_string_nul.lox"
    "test/translation-probes/06_shared_upvalue.lox"
    "test/translation-probes/V1_fresh_cell.lox"
    "test/translation-probes/V2_shared.lox"
    "test/translation-probes/V3_loopvar.lox"
    "test/translation-probes/V4_mutate_through_upvalue.lox"
    "test/translation-probes/V5_self_recursive_closure.lox"
    "test/translation-probes/V6_self_recursive_closure_in_loop.lox"
    "test/translation-probes/11_for_in.lox"
    "test/translation-probes/12_list_map_index.lox"
    "test/translation-probes/16_slice_in.lox"
    "test/translation-probes/25_seq_map_string_coverage.lox"
    "test/translation-probes/38_bound_native_method_identity.lox"
    # Classes, methods, super.
    "test/translation-probes/09_class.lox"
    "test/translation-probes/10_super.lox"
    "test/translation-probes/17_super_value.lox"
    "examples/class_dispatch.lox"
    "examples/shapes.lox"
    # Match/enum dispatch (GET_TAG, JUMP_TABLE, enum-ctor CONSTANT, the
    # loadNamedLocalAtZeroDepth fix for PRINT/DEFINE_GLOBAL of a bare match
    # result).
    "test/translation-probes/13_enum_match.lox"
    "test/translation-probes/14_enum_payload.lox"
    "examples/enum_match.lox"
    "examples/enum_result.lox"
    "examples/enum_tree.lox"
    "examples/match_dispatch.lox"
    "examples/match_http_status.lox"
    "examples/match_state_machine.lox"
    "examples/or_pattern_demo.lox"
    "examples/at_binding_demo.lox"
    "examples/string_list_pattern_demo.lox"
    # Run-parity proof for normalizeFoldedOperands's own required
    # coverage — the nine folded-operand shapes, RETURN of a folded match,
    # and a nested match subject.
    "test/translation-probes/28_folded_match_operand_family.lox"
    # Reflection introspection: type()/fields()/methods()/getField()/
    # hasField()/setField()/callMethod(), now supported on native, JVM, and
    # CLR alike (src/stdlib/reflect_api.cpp, LoxRuntime.registerReflection).
    "test/translation-probes/40_reflection.lox"
    # Visibility after close() (spec/05-stdlib.md, File section): the
    # differential corpus sweep alone reports a false MATCH when a probe
    # cannot even run (both sides give empty stdout and a matching non-zero
    # exit), so this probe also needs the exit-code-checked loop below.
    "test/translation-probes/48_file_visible_after_close.lox"
    # try/catch/throw/defer (PUSH_HANDLER/POP_HANDLER/THROW/DEFER_RECORD/
    # RUN_DEFERS). PR #237 rounds 1-3's own repros, plus round 3's shapes
    # (A)/(B) and their generalizations, plus issue #240 — none of these
    # were wired into an executed corpus before, which is why every one of
    # them survived to a live review instead of failing a build.
    "examples/simple_throw.lox"
    "examples/nested_try_catch.lox"
    "examples/test_throw_simple.lox"
    "examples/catch_index_error.lox"
    "examples/defer_throw_outer_catch.lox"
    "examples/defer_uncaught_throw.lox"
    "examples/defer_lifo.lox"
    "examples/try_catch_sibling_after_terminal_catch.lox"
    "examples/try_catch_local_in_catch_body.lox"
    "examples/try_catch_local_before_and_in_catch.lox"
    "examples/try_catch_three_terminal_siblings.lox"
    "examples/try_catch_defer_local_slot.lox"
    "examples/try_catch_invalid_receiver.lox"
    "examples/try_catch_nested_terminal_outer_catch.lox"
    "examples/try_catch_local_in_catch_then_sibling.lox"
    # Issue #253/#254: fault-classification fixes for JVM backend (catchable
    # error construction and Error instance property access).
    "examples/try_catch_class_constructor_arity.lox"
    "examples/try_catch_error_instance_properties_catchable.lox"
    "examples/try_catch_error_vs_ordinary_instance_catchability.lox"
    # Issue #268: LoxClosure's own frame-count ceiling delivers a catchable
    # StackOverflowError, matching native's kind, message, and post-catch
    # continuation. jvm-only/ because the CLR backend still delivers this
    # same fault with the wrong catchability as of this probe (issue #238).
    "test/translation-probes/jvm-only/catch_overflow.lox"
    # Issue #268: pins the success side of the frame-count ceiling boundary
    # (31_deep_recursion.lox below pins the failure side). Catches a
    # counter that starts too high and rejects a depth native accepts.
    "test/translation-probes/53_deep_recursion_boundary.lox"
    # Issue #268: a deferred call's own throw, while a StackOverflowError
    # unwinds, replaces it — the guard must still clear so a later,
    # unrelated overflow is caught.
    "test/translation-probes/jvm-only/defer_replaces_overflow.lox"
    # Mission #288 node #333: JVM fault sites wired against N1's completed
    # spec/04-semantics.md table (spec/03-types.md's Error section for the
    # message/kind fields). Each probe below pins one catchable row's exact
    # kind and message text, not only that some Error is delivered.
    "examples/try_catch_concatenation_type_error.lox"
    "examples/try_catch_undefined_property_on_error_message.lox"
    "examples/try_catch_invoke_on_error_invalid_receiver.lox"
    "examples/try_catch_set_index_not_indexable.lox"
    "examples/try_catch_match_error_message.lox"
    "examples/try_catch_undefined_variable_get_message.lox"
    "examples/try_catch_undefined_variable_set_message.lox"
    "examples/try_catch_ctor_arity_no_init_message.lox"
    "examples/try_catch_enum_ctor_arity_message.lox"
    "examples/try_catch_notcallable_ordinary_call.lox"
    "examples/try_catch_invalid_receiver_message.lox"
    "examples/try_catch_error_property_read_still_works.lox"
)

# Probes that must FAIL on both sides: a global function called before its
# own `fun` declaration has run is a
# late-bound-global error, not a silent success. Each entry needs a non-zero
# exit from build/loxpp AND from tools/loxpp_jvm.sh, with matching stdout —
# empty for 24, but 26 and 27 below each print something before they error,
# and that printed text must match exactly too.
error_probes=(
    "test/translation-probes/24_call_before_closure.lox"
    # Issue #268: LoxClosure's own frame-count ceiling. Both sides must
    # fail with empty stdout — native via src/vm.h's FRAMES_MAX, the JVM
    # backend via runtime/jvm/src/lox/LoxClosure.java's own counter.
    "test/translation-probes/31_deep_recursion.lox"
    # Issue #268: a StackOverflowError raised by a deferred call while
    # another one is still unwinding is fatal on both sides, per
    # spec/04-semantics.md line 1120 (not delivered to any catchBlock).
    "test/translation-probes/jvm-only/defer_overflow_during_unwind.lox"
    # Issue #268: a plain instance whose "kind" field spoofs
    # "StackOverflowError", thrown and caught entirely inside a deferred
    # call, must not clear the unwind guard early — the real overflow still
    # in progress must stay fatal against a second one, per
    # spec/04-semantics.md line 1120.
    "test/translation-probes/jvm-only/defer_overflow_kind_spoof.lox"
    # Issue #268: a deferred call's throw on a NORMAL return (no fault
    # propagating out of the deferring frame at all) must not be handed the
    # unwind guard's identity, even while an unrelated StackOverflowError
    # unwinds elsewhere in the program — the real overflow still in
    # progress must stay fatal against a second one, per
    # spec/04-semantics.md line 1120.
    "test/translation-probes/jvm-only/defer_throw_on_normal_return_during_unwind.lox"
    # Issue #268: a deferred call's throw that replaces a DIFFERENT,
    # unrelated fault (not the StackOverflowError unwinding elsewhere in
    # the program) must not be handed the unwind guard's identity — the
    # real overflow still in progress must stay fatal against a second one,
    # per spec/04-semantics.md line 1120.
    "test/translation-probes/jvm-only/defer_replaces_unrelated_fault_during_unwind.lox"
    # Every arm names a real constructor (so
    # checkEnumExhaustiveness accepts it as exhaustive) but every guard is
    # false at run time, so no arm actually accepts the value — a real,
    # reachable MATCH_ERROR, through the sparse compare-and-branch form (the
    # match carries a guard, so it is not table-eligible — see 27, below, for
    # the literal JUMP_TABLE default's own reachable case instead).
    "test/translation-probes/26_enum_match_dispatch_and_error.lox"
    # The literal JUMP_TABLE "out of range" fallthrough itself,
    # reached through a table-eligible match whose subject is a value of a
    # DIFFERENT enum than the arms name — see the probe's own header
    # comment.
    "test/translation-probes/27_jump_table_default_cross_enum.lox"
    # Reflection introspection guards: a non-Instance/non-Class argument to
    # each accessor is a runtime error on every backend, and callMethod()
    # v1 refuses a closure-backed method identically everywhere
    # (notes/expressiveness-roadmap.md item 1).
    "test/translation-probes/41_reflect_getfield_non_instance.lox"
    "test/translation-probes/42_reflect_setfield_non_instance.lox"
    "test/translation-probes/43_reflect_fields_non_instance.lox"
    "test/translation-probes/44_reflect_hasfield_non_instance.lox"
    "test/translation-probes/45_reflect_methods_non_instance.lox"
    "test/translation-probes/46_reflect_callmethod_non_instance.lox"
    "test/translation-probes/47_reflect_callmethod_closure_method.lox"
    # Mission #288 node #333: fault sites native routes through RAISE_ERROR
    # (fatal, never delivered to a catchBlock), which the JVM backend had
    # wired as catchable instead (over-catching) or left unguarded entirely
    # (the stringify depth guard, added for this node — see
    # spec/04-semantics.md's Fatal Runtime Errors section).
    "test/translation-probes/jvm-only/fault_set_property_on_error_fatal.lox"
    "test/translation-probes/jvm-only/fault_invoke_field_not_callable_fatal.lox"
    "test/translation-probes/jvm-only/fault_invoke_method_not_found_fatal.lox"
    "test/translation-probes/jvm-only/fault_enum_index_type_fatal.lox"
    "test/translation-probes/jvm-only/fault_enum_index_range_fatal.lox"
    "test/translation-probes/jvm-only/fault_set_index_string_fatal.lox"
    "test/translation-probes/jvm-only/fault_stringify_too_deep_print.lox"
    "test/translation-probes/jvm-only/fault_stringify_too_deep_in_try.lox"
    # Round-1 review of PR #345: a `defer`red call holding a non-callable
    # value, or a class construction, must be fatal (native's own
    # `runDefers`, src/vm.cpp lines 245-261), not delivered to any
    # catchBlock. Each probe puts the fault inside a try so a wrong,
    # catchable disposition prints and exits 0 instead of failing to run.
    "test/translation-probes/jvm-only/fault_defer_noncallable_value_fatal.lox"
    "test/translation-probes/jvm-only/fault_defer_class_construction_fatal.lox"
)

if [ ! -x "$native_bin" ]; then
    echo "check_jvm_probes.sh: no loxpp binary at $native_bin" >&2
    exit 1
fi

native_out="$(mktemp)"
native_err="$(mktemp)"
jvm_out="$(mktemp)"
jvm_err="$(mktemp)"
trap 'rm -f "$native_out" "$native_err" "$jvm_out" "$jvm_err"' EXIT

failed_probes=()
for probe in "${probes[@]}"; do
    if ! "$native_bin" "$root/$probe" >"$native_out" 2>"$native_err"; then
        echo "check_jvm_probes.sh: FAIL $probe (native run failed)" >&2
        cat "$native_err" >&2
        failed_probes+=("$probe")
        continue
    fi
    if ! "$root/tools/loxpp_jvm.sh" "$root/$probe" >"$jvm_out" 2>"$jvm_err"; then
        echo "check_jvm_probes.sh: FAIL $probe (JVM run failed)" >&2
        cat "$jvm_err" >&2
        failed_probes+=("$probe")
        continue
    fi
    if diff -u "$native_out" "$jvm_out"; then
        echo "check_jvm_probes.sh: OK $probe"
    else
        echo "check_jvm_probes.sh: FAIL $probe (stdout mismatch)" >&2
        failed_probes+=("$probe")
    fi
done

for probe in "${error_probes[@]}"; do
    "$native_bin" "$root/$probe" >"$native_out" 2>"$native_err"
    native_status=$?
    if [ "$native_status" -eq 0 ]; then
        echo "check_jvm_probes.sh: FAIL $probe (native run did not fail)" >&2
        failed_probes+=("$probe")
        continue
    fi

    # Emission and execution are two separate facts. A probe must fail at
    # RUN time, the same place the native side fails, not at emit time (an
    # unimplemented opcode, say). The combined
    # tools/loxpp_jvm.sh exits non-zero either way, so checking only its
    # exit code let an emit-time abort satisfy this loop by accident.
    j_dir="$(mktemp -d)"
    if ! "$native_bin" --target jvm --out-dir "$j_dir" "$root/$probe" \
        >/dev/null 2>"$jvm_err"; then
        echo "check_jvm_probes.sh: FAIL $probe (JVM emit failed, not a runtime error)" >&2
        cat "$jvm_err" >&2
        rm -rf "$j_dir"
        failed_probes+=("$probe")
        continue
    fi
    "$root/tools/jvm_run.sh" "$j_dir" "$lox_rt_jar" LoxMain \
        >"$jvm_out" 2>"$jvm_err"
    jvm_status=$?
    rm -rf "$j_dir"
    if [ "$jvm_status" -eq 0 ]; then
        echo "check_jvm_probes.sh: FAIL $probe (JVM run did not fail)" >&2
        failed_probes+=("$probe")
        continue
    fi

    # A non-zero exit is not proof of a Lox error. The JVM gives the same
    # non-zero exit for a host fault that happens before the Lox program can
    # raise anything: a missing or bad runtime jar, a class the verifier
    # rejects, a class that is not found, or a stack overflow. Only stderr
    # that names lox.LoxError proves the failure is the Lox-level error this
    # probe exists to check for.
    if ! grep -q "lox\.LoxError" "$jvm_err"; then
        echo "check_jvm_probes.sh: FAIL $probe (JVM run failed, but not with a lox.LoxError)" >&2
        cat "$jvm_err" >&2
        failed_probes+=("$probe")
        continue
    fi

    if diff -u "$native_out" "$jvm_out"; then
        echo "check_jvm_probes.sh: OK $probe (both failed, stdout matches)"
    else
        echo "check_jvm_probes.sh: FAIL $probe (stdout mismatch)" >&2
        failed_probes+=("$probe")
    fi
done

# --- JVM exclusion guard ---------------------------------------------------
# Re-proves, on every run, that each tools/jvm_excluded_examples.txt entry's
# JVM stdout is still a permutation of native stdout, not a stale byte-match
# or a content change that the exclusion is silently hiding. tools/diff_runtimes.py
# already does exactly this for tools/clr_excluded_examples.txt; --only-excluded
# runs it over exactly the excluded programs, resolved under examples/.
excluded_list="$root/tools/jvm_excluded_examples.txt"
if ! python3 "$root/tools/diff_runtimes.py" "$native_bin" \
        "$root/tools/loxpp_jvm.sh" --exclude "$excluded_list" \
        --only-excluded "$root/examples"; then
    echo "check_jvm_probes.sh: FAIL JVM exclusion guard (tools/jvm_excluded_examples.txt: stale or diverged entries)" >&2
    failed_probes+=("jvm_excluded_examples_permutation_guard")
else
    echo "check_jvm_probes.sh: JVM exclusion guard OK, every exclusion is still a map-order permutation"
fi

if [ "${#failed_probes[@]}" -ne 0 ]; then
    echo "check_jvm_probes.sh: ${#failed_probes[@]} probe(s) failed:" >&2
    for probe in "${failed_probes[@]}"; do
        echo "  $probe" >&2
    done
    exit 1
fi

echo "check_jvm_probes.sh: all $((${#probes[@]} + ${#error_probes[@]})) probes OK"
