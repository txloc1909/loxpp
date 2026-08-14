#!/usr/bin/env bash
#
# Assembles a directory of Jasmin (.j) files, then runs one class from the
# result against the JVM runtime library. This is the fixed harness a code
# generator plugs into: it does not know or care how the .j files were
# produced, so N4 and later nodes can call it unchanged.
#
# Usage: tools/jvm_run.sh <j-dir> <rt-jar> <main-class> [stack-size]
#
#   <j-dir>      directory holding one or more *.j files. Classes assemble
#                into this same directory, in a package subdirectory when a
#                .j file names one. Give a scratch directory: this harness
#                owns every .class file under <j-dir>, and deletes each one
#                it finds before it assembles a new set.
#   <rt-jar>     path to runtime/jvm/lox-rt.jar.
#   <main-class> fully qualified name of the class with
#                main([Ljava/lang/String;)V.
#   [stack-size] -Xss value, given as "64m" or "-Xss64m". Default 64m:
#                bootstrap/loxpp_interpreter.lox recurses deeply and
#                overflows the JVM's default thread stack.
#
# stdin, stdout, stderr, and the exit code all pass through to and from java
# unchanged, because later nodes diff this output against build/loxpp.
set -euo pipefail

if [ "$#" -lt 3 ]; then
    echo "usage: tools/jvm_run.sh <j-dir> <rt-jar> <main-class> [stack-size]" >&2
    exit 2
fi

j_dir="$1"
rt_jar="$2"
main_class="$3"
stack_size="${4:-64m}"
# Accept either a bare value ("64m") or a full flag ("-Xss64m"): a caller
# that passes the whole flag must not get "-Xss-Xss64m" (reported:
# jvm_run.sh, R4).
stack_size="${stack_size#-Xss}"

if [ ! -d "$j_dir" ]; then
    echo "jvm_run.sh: no such directory: $j_dir" >&2
    exit 1
fi

if [ ! -f "$rt_jar" ]; then
    echo "jvm_run.sh: no such runtime jar: $rt_jar" >&2
    echo "jvm_run.sh: run tools/build_lox_rt.sh first." >&2
    exit 1
fi

# Expand the glob ourselves. With no match, bash passes the literal "*.j"
# through unchanged, and jasmin given that one bad path — or truly zero
# arguments — prints its banner and exits non-zero with no useful message.
# Catch the empty case here and name the real problem instead.
j_files=("$j_dir"/*.j)
if [ ! -e "${j_files[0]}" ]; then
    echo "jvm_run.sh: no .j files in $j_dir" >&2
    exit 1
fi

# Clear class files left by an earlier run against this same directory. A
# stale one must not survive a failed run: below, we treat "no fresh .class"
# as failure, and a leftover file would defeat that check (reported:
# jvm_run.sh, R3). A packaged class assembles into a subdirectory of $j_dir
# (".class public gen/LoxMain" writes "$j_dir/gen/LoxMain.class"), so the
# scan must walk the whole tree, not only its top level (reported:
# jvm_run.sh, R5).
mapfile -d '' stale_classes < <(find "$j_dir" -name '*.class' -print0)
if [ "${#stale_classes[@]}" -gt 0 ]; then
    rm -f -- "${stale_classes[@]}"
fi

# Jasmin 2.4 can print "Found N errors" for a bad file and still exit 0,
# producing no .class for it (verified: a file with a genuine parse error
# assembles "successfully" by exit code alone). Exit code is not enough, so
# scan its own error summary line instead of the whole output: jasmin also
# prints "Generated: <path>" for each class, and <path> is caller-supplied
# (the j-dir argument), so a directory name that happens to contain "error"
# would falsely trip a scan of the whole output (reported: jvm_run.sh, R1).
jasmin_failed=0
if ! jasmin_out="$(jasmin -d "$j_dir" "${j_files[@]}" 2>&1)"; then
    jasmin_failed=1
elif printf '%s\n' "$jasmin_out" | grep -qE 'Found [0-9]+ errors?'; then
    jasmin_failed=1
fi

# Belt and suspenders: confirm jasmin actually wrote one class per input
# file, even when it reports no error. Combined with the stale-file cleanup
# above, a silent failure now leaves the directory empty, or short of the
# full set, instead of looking like a pass (reported: jvm_run.sh, R3). The
# tree walk matches the cleanup above, so a packaged class still counts
# (reported: jvm_run.sh, R5). Jasmin assembles exactly one class per .j
# file and prints one "Generated: <path>" line for it, so counting those
# lines against the input file count catches the case where one file among
# many silently produced nothing (reported: jvm_run.sh, R5).
if [ "$jasmin_failed" -eq 0 ]; then
    mapfile -d '' class_files < <(find "$j_dir" -name '*.class' -print0)
    if [ "${#class_files[@]}" -eq 0 ]; then
        jasmin_failed=1
        jasmin_out="${jasmin_out}
jvm_run.sh: jasmin reported no error but wrote no class files"
    else
        generated_count="$(printf '%s\n' "$jasmin_out" | grep -c '^Generated: ' || true)"
        if [ "$generated_count" -ne "${#j_files[@]}" ]; then
            jasmin_failed=1
            jasmin_out="${jasmin_out}
jvm_run.sh: expected ${#j_files[@]} class(es), one per .j file, but jasmin reported $generated_count"
        fi
    fi
fi

if [ "$jasmin_failed" -eq 1 ]; then
    echo "jvm_run.sh: jasmin assembly failed" >&2
    printf '%s\n' "$jasmin_out" >&2
    exit 1
fi

# exec, not a captured call: it replaces this script with java, so stdin,
# stdout, stderr, and the exit code are java's own, not a copy.
exec java -Xss"$stack_size" -cp "$rt_jar:$j_dir" "$main_class"
