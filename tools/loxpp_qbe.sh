#!/usr/bin/env bash
#
# Compiles and runs one Lox++ program on the QBE backend. One command, in
# place of `build/loxpp program.lox`, so tools/diff_runtimes.py can diff its
# output against the native binary (node S7, issue #460).
#
# Usage: tools/loxpp_qbe.sh program.lox [arg...]
#
# The front end is `loxpp --target qbe -o <exe> program.lox` (src/main.cpp,
# backend/qbe_frontend.cpp). This script is only a thin driver: it resolves
# the loxpp binary, picks a scratch output path, runs the front end, then
# runs the produced executable. stdin, stdout, and stderr pass through
# unchanged; the executable's exit status is this script's exit status.
#
# QBE_NO_PROMOTE=1 and QBE_NO_FUSE=1 select the pre-S8 emitter, so
# benchmarks/run.py and tools/check_qbe_s8_promotion.sh can compare a
# baseline against register promotion or the GET_TAG;JUMP_TABLE fusion in
# one build.
#
# QBE, CXX, LOX_RT_A, and LOXPP_BIN override the default tool and binary
# locations, the same env vars the front end itself reads.
#
# Build first, release preset (AGENTS.md's warning on the debug preset's
# trace output applies here too):
#   cmake --preset release && cmake --build build \
#       --target loxpp loxrt -j$(nproc)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
loxpp_bin="${LOXPP_BIN:-$root/build/loxpp}"

if [ "$#" -lt 1 ]; then
    echo "usage: tools/loxpp_qbe.sh program.lox [arg...]" >&2
    exit 2
fi
program="$1"; shift

if [ ! -x "$loxpp_bin" ]; then
    echo "loxpp_qbe.sh: no loxpp binary at $loxpp_bin (build it, or set LOXPP_BIN)" >&2
    exit 1
fi
if [ ! -f "$program" ]; then
    echo "loxpp_qbe.sh: no such file: $program" >&2
    exit 1
fi

emit_flags=()
if [ -n "${QBE_NO_PROMOTE:-}" ]; then
    emit_flags+=(--no-promote)
fi
if [ -n "${QBE_NO_FUSE:-}" ]; then
    emit_flags+=(--no-fuse)
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

program_abs="$(cd "$(dirname "$program")" && pwd)/$(basename "$program")"

"$loxpp_bin" --target qbe ${emit_flags[@]+"${emit_flags[@]}"} -o "$work/prog" "$program_abs"

"$work/prog" "$@"
