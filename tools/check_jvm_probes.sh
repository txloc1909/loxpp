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
    # R21: run-parity proof for normalizeFoldedOperands's own required
    # coverage — the nine R15 shapes, RETURN of a folded match, and a nested
    # match subject.
    "test/translation-probes/28_folded_match_operand_family.lox"
)

# Probes that must FAIL on both sides: a global function called before its
# own `fun` declaration has run is a
# late-bound-global error, not a silent success. Each entry needs a non-zero
# exit from build/loxpp AND from tools/loxpp_jvm.sh, with matching stdout —
# empty for 24, but 26 and 27 below each print something before they error,
# and that printed text must match exactly too.
error_probes=(
    "test/translation-probes/24_call_before_closure.lox"
    # Every arm names a real constructor (so
    # checkEnumExhaustiveness accepts it as exhaustive) but every guard is
    # false at run time, so no arm actually accepts the value — a real,
    # reachable MATCH_ERROR, through the sparse compare-and-branch form (the
    # match carries a guard, so it is not table-eligible — see 27, below, for
    # the literal JUMP_TABLE default's own reachable case instead).
    "test/translation-probes/26_enum_match_dispatch_and_error.lox"
    # R1 fix: the literal JUMP_TABLE "out of range" fallthrough itself,
    # reached through a table-eligible match whose subject is a value of a
    # DIFFERENT enum than the arms name — see the probe's own header
    # comment.
    "test/translation-probes/27_jump_table_default_cross_enum.lox"
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
