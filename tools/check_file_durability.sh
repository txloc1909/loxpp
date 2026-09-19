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
# The p6 shape ends in an uncaught throw and exits non-zero by design. This
# script must not require exit 0 to check that shape's file - skipping a
# non-zero run and calling that a pass is exactly the false green
# tools/check_clr_probes.sh's corpus sweep is documented to give for a run
# that never reaches exit 0.
#
# Usage: tools/check_file_durability.sh <runner>
#
#   <runner>   runs one Lox++ program, invoked as "<runner> program.lox",
#              inheriting stdout/stderr. build/loxpp, tools/loxpp_jvm.sh,
#              tools/loxpp_clr.sh, and bootstrap/lox_wrapper.sh (export
#              LANGUAGE=LOXPP first) all match this interface.
set -uo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: tools/check_file_durability.sh <runner>" >&2
    exit 2
fi
runner="$1"

failed_probes=()

# Writes $program_path, runs it through $runner, then compares the file it
# wrote against $expected_content with cmp. $file_path must not exist yet -
# a runner that fails to write it at all is reported by name, not confused
# with a content mismatch.
check_probe() {
    local probe_name="$1" expected_content="$2" program_path="$3" file_path="$4"
    local dir out err expected_path status
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
    if cmp -s "$expected_path" "$file_path"; then
        echo "check_file_durability.sh: OK $probe_name (runner exit=$status, $(wc -c <"$file_path" | tr -d ' ') bytes)"
    else
        echo "check_file_durability.sh: FAIL $probe_name (runner exit=$status, content mismatch)" >&2
        echo "  expected: $(printf '%q' "$expected_content")" >&2
        echo "  actual:   $(printf '%q' "$(cat "$file_path" 2>/dev/null)")" >&2
        failed_probes+=("$probe_name")
    fi
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
check_probe "p5_exit_normal" $'survives-normal-exit\n' "$program_path" "$file_path"
rm -rf "$dir"

# p5b: no close(), explicit exit(0).
dir="$(mktemp -d)"
file_path="$dir/p5b.txt"
program_path="$dir/p5b_exit_call.lox"
cat >"$program_path" <<EOF
var w = open("$file_path", "w");
w.writeline("survives-exit-call");
print "p5b before exit";
exit(0);
EOF
check_probe "p5b_exit_call" $'survives-exit-call\n' "$program_path" "$file_path"
rm -rf "$dir"

# p6: no close(), uncaught throw. Exits non-zero by design (see header).
dir="$(mktemp -d)"
file_path="$dir/p6.txt"
program_path="$dir/p6_uncaught_throw_noclose.lox"
cat >"$program_path" <<EOF
var w = open("$file_path", "w");
w.writeline("survives-uncaught-throw");
print "p6 before throw";
throw "boom";
EOF
check_probe "p6_uncaught_throw_noclose" $'survives-uncaught-throw\n' "$program_path" "$file_path"
rm -rf "$dir"

if [ "${#failed_probes[@]}" -ne 0 ]; then
    echo "check_file_durability.sh: ${#failed_probes[@]} probe(s) failed:" >&2
    for probe in "${failed_probes[@]}"; do
        echo "  $probe" >&2
    done
    exit 1
fi

echo "check_file_durability.sh: all 3 probes OK"
