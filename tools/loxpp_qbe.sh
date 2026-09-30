#!/usr/bin/env bash
#
# Compiles and runs one Lox++ program on the QBE backend. One command, in
# place of `build/loxpp program.lox`, so tools/diff_runtimes.py can diff its
# output against the native binary (node S7, issue #460).
#
# Usage: tools/loxpp_qbe.sh program.lox
#
# Unlike tools/loxpp_jvm.sh/loxpp_clr.sh, there is no `loxpp --target qbe`
# front end yet (notes/qbe-backend.md's Staged plan never scheduled one — S2
# through S6 all drive the pipeline through qbe_emit_program plus a
# generated harness, the same shape this script automates). This script is
# that missing front end, built from the exact steps
# tools/check_qbe_s6_errors.sh already runs per probe:
#
#   qbe_emit_program program.lox   -> whole-program .ssa (stdout) plus one
#                                      "<id> <arity> <chunkHash> <symbol>"
#                                      RtFunctionDesc line per function
#                                      (stderr)
#   a generated harness.cpp        -> extern "C" decls + an RtFunctionDesc
#                                      array from those lines, calling
#                                      rt_startup(source, descs, ...,
#                                      requireAllCompiled=true) then
#                                      rt_call(rt, 0)
#   qbe                            -> assembles the .ssa to a .s
#   c++                            -> links the .s, the harness, and
#                                      libloxrt.a into one executable
#
# requireAllCompiled=true (rt_capi.h's own comment on why) matters here more
# than in any single node's own checkpoint: a whole, unknown corpus program
# can reach ANY opcode, so a silent interpreter fallback must not happen —
# rt_startup refuses to start rather than leave an uncompiled function for
# nothing to ever run (see rt_capi.h's own comment on the CallFrame it would
# otherwise leave dangling).
#
# stdin and stdout pass through unchanged: fds are inherited, not
# redirected. The exit code passes through too (0 on success, 70 on an
# uncaught Lox++ runtime error, matching native's own RUNTIME_ERROR code —
# see check_qbe_s6_errors.sh's own comment on why this convention is
# unchanged from S3-S6), but not via `exec`: this script owns a scratch
# build directory it must remove first, so the run is an ordinary last
# command and `set -e` (failure) or fall-through (success) carries its exit
# status out, with the EXIT trap firing either way.
#
# QBE_EMIT_PROGRAM, LOX_RT_A, and QBE override the default binary/tool
# locations, for a caller that built into a non-default directory or wants a
# non-default `qbe` on PATH.
#
# Build first, release preset (AGENTS.md's warning on the debug preset's
# trace output applies here too):
#   cmake --preset release && cmake --build build \
#       --target loxpp qbe_emit_program loxrt -j$(nproc)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
qbe_emit_program="${QBE_EMIT_PROGRAM:-$root/build/qbe_emit_program}"
libloxrt="${LOX_RT_A:-$root/build/libloxrt.a}"
qbe_bin="${QBE:-qbe}"

if [ "$#" -lt 1 ]; then
    echo "usage: tools/loxpp_qbe.sh program.lox" >&2
    exit 2
fi
program="$1"

if [ ! -x "$qbe_emit_program" ]; then
    echo "loxpp_qbe.sh: no executable at $qbe_emit_program (build it, or set QBE_EMIT_PROGRAM)" >&2
    exit 1
fi
if [ ! -f "$libloxrt" ]; then
    echo "loxpp_qbe.sh: $libloxrt not found (build it, or set LOX_RT_A)" >&2
    exit 1
fi
if [ ! -f "$program" ]; then
    echo "loxpp_qbe.sh: no such file: $program" >&2
    exit 1
fi
if ! command -v "$qbe_bin" >/dev/null 2>&1; then
    echo "loxpp_qbe.sh: no '$qbe_bin' on PATH (set QBE)" >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

program_abs="$(cd "$(dirname "$program")" && pwd)/$(basename "$program")"

if ! "$qbe_emit_program" "$program_abs" >"$work/prog.ssa" 2>"$work/prog.descs"; then
    echo "loxpp_qbe.sh: qbe_emit_program failed on $program" >&2
    cat "$work/prog.descs" >&2
    exit 1
fi

{
    echo '#include "backend/rt_abi.h"'
    echo '#include "backend/rt_capi.h"'
    echo '#include <cstdio>'
    echo '#include <fstream>'
    echo '#include <sstream>'
    echo '#include <string>'
    echo
    while read -r id arity hash symbol; do
        echo "extern \"C\" int ${symbol}(Runtime*, Value*);"
    done <"$work/prog.descs"
    echo
    echo 'int main(int argc, char** argv) {'
    echo '    if (argc < 2) { std::fprintf(stderr, "usage: harness program.lox\n"); return 64; }'
    echo '    std::ifstream file(argv[1]);'
    echo '    if (!file) { std::fprintf(stderr, "harness: cannot read %s\n", argv[1]); return 74; }'
    echo '    std::stringstream ss; ss << file.rdbuf();'
    echo '    std::string source = ss.str();'
    echo '    RtFunctionDesc descs[] = {'
    while read -r id arity hash symbol; do
        echo "        {\"${id}\", ${arity}, ${hash}ULL, reinterpret_cast<void*>(&${symbol})},"
    done <"$work/prog.descs"
    echo '    };'
    echo '    Runtime* rt = rt_startup(source.c_str(), descs, sizeof(descs) / sizeof(descs[0]), true);'
    echo '    if (rt == nullptr) { return 65; }'
    echo '    int status = rt_call(rt, 0);'
    echo '    rt_shutdown(rt);'
    echo '    return status == 0 ? 0 : 70;'
    echo '}'
} >"$work/harness.cpp"

if ! "$qbe_bin" -o "$work/prog.s" "$work/prog.ssa" 2>"$work/qbe_err"; then
    echo "loxpp_qbe.sh: qbe failed on $program" >&2
    cat "$work/qbe_err" >&2
    exit 1
fi

if ! c++ -std=c++17 -O2 -I "$root/src" "$work/harness.cpp" "$work/prog.s" \
         "$libloxrt" -lstdc++ -lm -o "$work/prog_bin" 2>"$work/cc_err"; then
    echo "loxpp_qbe.sh: link failed on $program" >&2
    cat "$work/cc_err" >&2
    exit 1
fi

"$work/prog_bin" "$program_abs"
