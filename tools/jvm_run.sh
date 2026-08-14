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
#                into this same directory.
#   <rt-jar>     path to runtime/jvm/lox-rt.jar.
#   <main-class> fully qualified name of the class with
#                main([Ljava/lang/String;)V.
#   [stack-size] -Xss value. Default 64m: bootstrap/loxpp_interpreter.lox
#                recurses deeply and overflows the JVM's default thread stack.
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

# Jasmin 2.4 can print "Found N errors" for a bad file and still exit 0,
# producing no .class for it (verified: a file with a genuine parse error
# assembles "successfully" by exit code alone). Exit code is not enough, so
# also scan its own error summary. A clean run's only output line is
# "Generated: <path>" per class, so this does not fire on success.
jasmin_failed=0
if ! jasmin_out="$(jasmin -d "$j_dir" "${j_files[@]}" 2>&1)"; then
    jasmin_failed=1
elif printf '%s\n' "$jasmin_out" | grep -qi 'error'; then
    jasmin_failed=1
fi

if [ "$jasmin_failed" -eq 1 ]; then
    echo "jvm_run.sh: jasmin assembly failed" >&2
    printf '%s\n' "$jasmin_out" >&2
    exit 1
fi

# exec, not a captured call: it replaces this script with java, so stdin,
# stdout, stderr, and the exit code are java's own, not a copy.
exec java -Xss"$stack_size" -cp "$rt_jar:$j_dir" "$main_class"
