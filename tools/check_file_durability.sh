#!/usr/bin/env bash
#
# spec/05-stdlib.md's File section guarantees that every File still open
# when the program ends - end of script, exit(), or an uncaught throw - is
# flushed as if close() had been called. close() itself already flushes on
# every consumer, so this checks the File a program forgets to close, which
# is the shape a naively buffered write loses.
#
# Runs three probes, one per exit path, each in its own fresh temporary
# directory so a stale file from an earlier invocation can never be mistaken
# for a real pass. Every probe's file is checked with cmp against the exact
# expected bytes, not just a length, so a right-sized but wrong-content file
# still fails.
#
# The p6 shape ends in an uncaught throw and exits non-zero, differently on
# each consumer (70 native, 1 JVM, 134 CLR, 70 bootstrap), so it cannot be
# checked against one fixed exit code the way p5/p5b are. It is still checked
# against "non-zero", not left with no status rule at all - an exit of 0
# there means the run took the normal-exit path instead of the
# uncaught-fault path this probe exists to cover, and every consumer today
# gives a non-zero code for it, so this rule costs nothing. Skipping the
# status check entirely and trusting file content alone is exactly the false
# green tools/check_clr_probes.sh's corpus sweep is documented to give for a
# run that never reaches exit 0. p5 and p5b, by contrast, both end without an
# uncaught fault, so both are checked against exit 0 - a probe whose file
# content happens to match by taking a *different* path than the one it
# names (see p5b_exit_call below) must not be reported as an unqualified OK.
#
# Both the exit() and the throw shapes also carry a sentinel write past the
# point where the program must end. A consumer where exit() (or the throw)
# does nothing runs on to the sentinel and then fails on content, so the
# probe measures that the program ended where it names, not only that the
# first line reached the file with a matching status.
#
# Usage: tools/check_file_durability.sh <runner> [--no-exit-builtin]
#
#   <runner>            runs one Lox++ program, invoked as
#                       "<runner> program.lox", inheriting stdout/stderr.
#                       build/loxpp, tools/loxpp_jvm.sh, tools/loxpp_clr.sh,
#                       and bootstrap/lox_wrapper.sh (export LANGUAGE=LOXPP
#                       first) all match this interface.
#   --no-exit-builtin   this consumer's stdlib has no exit() (a separately
#                       tracked, accepted gap, out of scope here); the
#                       script first runs "exit(0);" and requires the
#                       undefined-variable fault, so a consumer that does
#                       have exit() fails here instead of taking a quiet
#                       SKIP. Only after that proof does the p5b_exit_call
#                       probe report SKIP rather than run at all - p6 already
#                       covers the uncaught-fault flush path this consumer
#                       falls back to instead.
set -uo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: tools/check_file_durability.sh <runner> [--no-exit-builtin]" >&2
    exit 2
fi
runner="$1"
no_exit_builtin=0
if [ "$#" -eq 2 ]; then
    if [ "$2" != "--no-exit-builtin" ]; then
        echo "usage: tools/check_file_durability.sh <runner> [--no-exit-builtin]" >&2
        exit 2
    fi
    no_exit_builtin=1
fi

failed_probes=()
passed_probes=()
skipped_probes=()

# Writes $program_path, runs it through $runner, then compares the file it
# wrote against $expected_content with cmp. $file_path must not exist yet -
# a runner that fails to write it at all is reported by name, not confused
# with a content mismatch. $expected_status is the runner
# exit status this probe's own shape requires (0 for a script that ends
# without a fault); a probe whose file content matches by taking a
# different path is a wrong-reason pass, not a real one, so a status
# mismatch fails the probe even when cmp agrees. Pass "nonzero" for a probe
# whose own shape requires a fault exit but whose exact code is allowed to
# vary by consumer (p6, by design - see the header): an exit of 0 there means
# the run took the normal-exit path instead of the uncaught-fault path this
# probe exists to cover, so it fails the probe even though the file content
# still matches. The status rule itself is also checked: only a number or
# "nonzero" is accepted. Any other value (including empty) exits the script
# with an internal error instead of running the probe - a typo such as "O"
# for "0" must not silently reopen the no-status hole this rule closes.
check_probe() {
    local probe_name="$1" expected_content="$2" program_path="$3" file_path="$4" expected_status="${5:-}"
    local dir out err expected_path status
    case "$expected_status" in
        nonzero)
            ;;
        ''|*[!0-9]*)
            echo "check_file_durability.sh: internal error: check_probe $probe_name has bad expected_status '$expected_status'" >&2
            exit 2
            ;;
    esac
    dir="$(dirname "$program_path")"
    out="$dir/stdout"
    err="$dir/stderr"

    "$runner" "$program_path" >"$out" 2>"$err"
    status=$?

    if [ ! -f "$file_path" ]; then
        echo "check_file_durability.sh: FAIL $probe_name (no file written; runner exit=$status)" >&2
        sed 's/^/  stderr: /' "$err" >&2
        failed_probes+=("$probe_name")
        return
    fi

    expected_path="$dir/expected"
    printf '%s' "$expected_content" >"$expected_path"
    if ! cmp -s "$expected_path" "$file_path"; then
        echo "check_file_durability.sh: FAIL $probe_name (runner exit=$status, content mismatch)" >&2
        echo "  expected: $(printf '%q' "$expected_content")" >&2
        echo "  actual:   $(printf '%q' "$(cat "$file_path" 2>/dev/null)")" >&2
        failed_probes+=("$probe_name")
        return
    fi

    if [ "$expected_status" = "nonzero" ]; then
        if [ "$status" -eq 0 ]; then
            echo "check_file_durability.sh: FAIL $probe_name (expected a non-zero runner exit, got exit=0; file content matched anyway, so this probe took the normal-exit path instead of the uncaught-fault path it names)" >&2
            failed_probes+=("$probe_name")
            return
        fi
    elif [ "$status" -ne "$expected_status" ]; then
        echo "check_file_durability.sh: FAIL $probe_name (expected runner exit=$expected_status, got exit=$status; file content matched anyway, so this probe took a different path than the one it names)" >&2
        failed_probes+=("$probe_name")
        return
    fi

    passed_probes+=("$probe_name")
    echo "check_file_durability.sh: OK $probe_name (runner exit=$status, $(wc -c <"$file_path" | tr -d ' ') bytes)"
}

# Reports $probe_name as skipped, with $reason, instead of running it at
# all. A skip is loud and named - never a silent OK for the wrong reason -
# and does not count as a failure, matching tools/check_examples.py's own
# SKIP outcome for an example excluded for a stated, known reason.
skip_probe() {
    local probe_name="$1" reason="$2"
    echo "check_file_durability.sh: SKIP $probe_name ($reason)"
    skipped_probes+=("$probe_name")
}

# p5: no close(), normal end of script.
dir="$(mktemp -d)"
file_path="$dir/p5.txt"
program_path="$dir/p5_exit_normal.lox"
cat >"$program_path" <<EOF
var w = open("$file_path", "w");
w.writeline("survives-normal-exit");
print "p5 done";
EOF
check_probe "p5_exit_normal" $'survives-normal-exit\n' "$program_path" "$file_path" 0
rm -rf "$dir"

# p5b: no close(), explicit exit(0). Skipped on a consumer with no exit()
# builtin - there, the program would instead end in an uncaught "undefined
# variable" fault, the same path p6 already covers, and reporting that as
# an OK exit(0) pass would be the wrong-reason pass this script exists to
# catch (see the header). The flag is measured, not trusted: "exit(0);"
# must fail with the undefined-variable fault first. The sentinel write
# after exit(0) measures that the program ended at the exit call - a
# consumer where exit() does nothing runs on to it and fails on content.
if [ "$no_exit_builtin" -eq 1 ]; then
    verify_dir="$(mktemp -d)"
    verify_program="$verify_dir/verify_no_exit.lox"
    printf '%s\n' 'exit(0);' >"$verify_program"
    "$runner" "$verify_program" >"$verify_dir/stdout" 2>"$verify_dir/stderr"
    verify_status=$?
    if [ "$verify_status" -eq 0 ] || ! grep -q "Undefined variable 'exit'" "$verify_dir/stderr"; then
        echo "check_file_durability.sh: FAIL p5b_exit_call (consumer was run with --no-exit-builtin but exit(0) did not fail with the undefined-variable fault; runner exit=$verify_status)" >&2
        sed 's/^/  stderr: /' "$verify_dir/stderr" >&2
        failed_probes+=("p5b_exit_call")
    else
        skip_probe "p5b_exit_call" "consumer has no exit() builtin; p6 already covers the uncaught-fault flush path this shape would otherwise fall back to"
    fi
    rm -rf "$verify_dir"
else
    dir="$(mktemp -d)"
    file_path="$dir/p5b.txt"
    program_path="$dir/p5b_exit_call.lox"
    cat >"$program_path" <<EOF
var w = open("$file_path", "w");
w.writeline("survives-exit-call");
print "p5b before exit";
exit(0);
w.writeline("UNREACHABLE-PAST-EXIT");
EOF
    check_probe "p5b_exit_call" $'survives-exit-call\n' "$program_path" "$file_path" 0
    rm -rf "$dir"
fi

# p6: no close(), uncaught throw. Exits non-zero by design (see header).
# The sentinel write after the throw measures that the throw ended the
# program - a consumer where the throw does nothing runs on to it and fails
# on content.
dir="$(mktemp -d)"
file_path="$dir/p6.txt"
program_path="$dir/p6_uncaught_throw_noclose.lox"
cat >"$program_path" <<EOF
var w = open("$file_path", "w");
w.writeline("survives-uncaught-throw");
print "p6 before throw";
throw "boom";
w.writeline("UNREACHABLE-PAST-THROW");
EOF
check_probe "p6_uncaught_throw_noclose" $'survives-uncaught-throw\n' "$program_path" "$file_path" nonzero
rm -rf "$dir"

if [ "${#failed_probes[@]}" -ne 0 ]; then
    echo "check_file_durability.sh: ${#failed_probes[@]} probe(s) failed:" >&2
    for probe in "${failed_probes[@]}"; do
        echo "  $probe" >&2
    done
    exit 1
fi

if [ "${#skipped_probes[@]}" -ne 0 ]; then
    echo "check_file_durability.sh: all ${#passed_probes[@]} run probe(s) OK, ${#skipped_probes[@]} skipped"
else
    echo "check_file_durability.sh: all ${#passed_probes[@]} probes OK"
fi
