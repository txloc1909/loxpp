#!/usr/bin/env bash
#
# Compiles and runs one Lox++ program on the CLR backend: loxpp --target
# clr, then tools/clr_run.sh. One command, in place of `build/loxpp
# program.lox`, so later checks can diff its output against the native
# binary.
#
# Usage: tools/loxpp_clr.sh program.lox [arg...]
#
#   program.lox   the Lox++ source file to run.
#   [arg...]      program arguments, forwarded to tools/clr_run.sh, which
#                 hands them to dotnet so the native `args()` global
#                 answers them (see its own note).
#
# stdin, stdout, and stderr all pass through unchanged: fds are inherited,
# not redirected. The exit code passes through too, but not via `exec` —
# this script owns a scratch directory it must remove first, so it runs
# tools/clr_run.sh as an ordinary last command and lets `set -e` (on
# failure) or plain fall-through (on success) carry that command's own
# status out as this script's status, with the EXIT trap below firing
# either way.
#
# LOXPP_BIN and LOX_RT_CLR_DLL override the default binary/dll locations,
# for a caller that built into a non-default location.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
loxpp_bin="${LOXPP_BIN:-$root/build/loxpp}"
rt_dll="${LOX_RT_CLR_DLL:-$root/runtime/clr/LoxRuntime.dll}"

if [ "$#" -lt 1 ]; then
    echo "usage: tools/loxpp_clr.sh program.lox [arg...]" >&2
    exit 2
fi
program="$1"
program_args=("${@:2}")

if [ ! -x "$loxpp_bin" ]; then
    echo "loxpp_clr.sh: no loxpp binary at $loxpp_bin (build it, or set LOXPP_BIN)" >&2
    exit 1
fi
if [ ! -f "$program" ]; then
    echo "loxpp_clr.sh: no such file: $program" >&2
    exit 1
fi

il_dir="$(mktemp -d)"
trap 'rm -rf "$il_dir"' EXIT

"$loxpp_bin" --target clr --out-dir "$il_dir" "$program"

"$root/tools/clr_run.sh" "$il_dir" "$rt_dll" LoxMain "${program_args[@]}"
