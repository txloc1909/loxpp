#!/usr/bin/env bash
#
# Compiles and runs one Lox++ program on the JVM backend: loxpp --target jvm,
# then tools/jvm_run.sh. One command, in place of `build/loxpp program.lox`,
# so later nodes and CI can diff its output against the native binary.
#
# Usage: tools/loxpp_jvm.sh program.lox [stack-size]
#
#   program.lox   the Lox++ source file to run.
#   [stack-size]  -Xss value passed through to tools/jvm_run.sh. Default 64m.
#
# stdin, stdout, and stderr all pass through unchanged: fds are inherited,
# not redirected. The exit code passes through too, but not via `exec` — this
# script owns a scratch directory it must remove first, so it runs
# tools/jvm_run.sh as an ordinary last command and lets `set -e` (on failure)
# or plain fall-through (on success) carry that command's own status out as
# this script's status, with the EXIT trap below firing either way.
#
# LOXPP_BIN and LOX_RT_JAR override the default binary/jar locations, for a
# caller that built into a non-default directory.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
loxpp_bin="${LOXPP_BIN:-$root/build/loxpp}"
rt_jar="${LOX_RT_JAR:-$root/runtime/jvm/lox-rt.jar}"

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: tools/loxpp_jvm.sh program.lox [stack-size]" >&2
    exit 2
fi
program="$1"
stack_size="${2:-64m}"

if [ ! -x "$loxpp_bin" ]; then
    echo "loxpp_jvm.sh: no loxpp binary at $loxpp_bin (build it, or set LOXPP_BIN)" >&2
    exit 1
fi
if [ ! -f "$program" ]; then
    echo "loxpp_jvm.sh: no such file: $program" >&2
    exit 1
fi

j_dir="$(mktemp -d)"
trap 'rm -rf "$j_dir"' EXIT

"$loxpp_bin" --target jvm --out-dir "$j_dir" "$program"

"$root/tools/jvm_run.sh" "$j_dir" "$rt_jar" LoxMain "$stack_size"
