#!/usr/bin/env bash
#
# Assembles a directory of Jasmin (.j) files, then runs one class from the
# result against the JVM runtime library. This is the fixed harness a code
# generator plugs into: it does not know or care how the .j files were
# produced, so any emitter code can call it unchanged.
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

if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
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

# Derive the class file each .j source must produce, from that source's own
# ".class" directive, rather than trusting jasmin's exit code or its message
# text afterward. Text- or count-based proof can say "assembly succeeded"
# when it had not, three different ways: a directory name that matched the
# error scan (R1), a packaged class that a shallow scan missed (R5), and two
# files naming one class, which still print one "Generated:" line each even
# though the second overwrites the first on disk (R8). Checking the exact
# expected path is immune to all three, by construction, because it does
# not read jasmin's output at all.
#
# A ".class" directive reads "<access-spec...> <name>"; <name> is always
# the last field, and may hold a package path ("gen/LoxMain"). Two files
# that derive the same name are a source-level collision: report it before
# jasmin runs, because jasmin does not reject that across separate files,
# only within one file.
declare -A owner_of_class=()
derive_failed=0
for f in "${j_files[@]}"; do
    class_name="$(awk '
        /^[[:space:]]*\.class[[:space:]]/ { sub(/;.*/, ""); print $NF; exit }
    ' "$f")"
    if [ -z "$class_name" ]; then
        echo "jvm_run.sh: $f has no .class directive" >&2
        derive_failed=1
    elif [ -n "${owner_of_class[$class_name]+set}" ]; then
        echo "jvm_run.sh: ${owner_of_class[$class_name]} and $f both declare class $class_name" >&2
        derive_failed=1
    else
        owner_of_class[$class_name]="$f"
    fi
done
if [ "$derive_failed" -eq 1 ]; then
    echo "jvm_run.sh: jasmin assembly failed" >&2
    exit 1
fi

# Clear class files left by an earlier run against this same directory, so
# a leftover file cannot stand in for one jasmin was supposed to write this
# time. A packaged class assembles into a subdirectory of $j_dir, so the
# scan must walk the whole tree, not only its top level (reported:
# jvm_run.sh, R5). A trailing slash makes find enter $j_dir even when the
# caller passes it as a symbolic link, which find would not otherwise
# follow as a path argument (reported: jvm_run.sh, R9).
mapfile -d '' stale_classes < <(find "$j_dir"/ -name '*.class' -print0)
if [ "${#stale_classes[@]}" -gt 0 ]; then
    rm -f -- "${stale_classes[@]}"
fi

# Jasmin's own exit code is not proof of anything: a file with a genuine
# parse error still assembles "successfully" by exit code alone (verified).
# The real proof is the check below, so run jasmin and keep its output only
# to show the caller, on failure, not to judge success by.
jasmin_out="$(jasmin -d "$j_dir" "${j_files[@]}" 2>&1)" || true

# The one thing that must be true: every class a source declared exists at
# the exact path its own ".class" directive named. This test needs no fact
# about jasmin's output, so a false pass in that output cannot defeat it.
missing_failed=0
for class_name in "${!owner_of_class[@]}"; do
    if [ ! -f "$j_dir/$class_name.class" ]; then
        echo "jvm_run.sh: expected class file $j_dir/$class_name.class, from ${owner_of_class[$class_name]}, was not written" >&2
        missing_failed=1
    fi
done
if [ "$missing_failed" -eq 1 ]; then
    echo "jvm_run.sh: jasmin assembly failed" >&2
    printf '%s\n' "$jasmin_out" >&2
    exit 1
fi

# exec, not a captured call: it replaces this script with java, so stdin,
# stdout, stderr, and the exit code are java's own, not a copy.
exec java -Xss"$stack_size" -cp "$rt_jar:$j_dir" "$main_class"
