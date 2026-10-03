#!/usr/bin/env bash
#
# Round-trips the S5 node's checkpoint (issue #458): probes 09-14 and 16
# (classes, super, for-in, lists/maps/index, enum match with a dense tag
# and a payload, slice/in) must be byte-identical to native (stdout + exit
# status). Same whole-program driver as S4's own checkpoint
# (check_qbe_s4_calls_closures.sh) — see that script's own file comment for
# why: qbe_emit_program compiles the top-level script plus every function it
# reaches, one QBE .ssa translation unit, plus one "<id> <arity> <chunkHash>
# <qbeSymbol>" descriptor line per function on stderr.
#
# Build with the release preset first (AGENTS.md): the debug preset leaves
# LOXPP_DEBUG_PRINT_CODE/LOXPP_DEBUG_TRACE_EXECUTION on by default, so
# native's own stdout would carry a bytecode trace no QBE-compiled binary
# ever produces, and every probe would "diverge" even though nothing is
# wrong.
#
#   cmake --preset release && cmake --build build \
#       --target loxpp qbe_emit_program loxrt -j$(nproc)
#   tools/check_qbe_s5_rest_of_language.sh
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
loxpp_bin="${LOXPP_BIN:-$root/build/loxpp}"
qbe_emit_program="${QBE_EMIT_PROGRAM:-$root/build/qbe_emit_program}"
libloxrt="${LOX_RT_A:-$root/build/libloxrt.a}"

for bin in "$loxpp_bin" "$qbe_emit_program"; do
    if [ ! -x "$bin" ]; then
        echo "check_qbe_s5_rest_of_language: no executable at $bin (build it first)" >&2
        exit 1
    fi
done
if [ ! -f "$libloxrt" ]; then
    echo "check_qbe_s5_rest_of_language: $libloxrt not found (build it first)" >&2
    exit 1
fi

probes=(
    09_class
    10_super
    11_for_in
    12_list_map_index
    13_enum_match
    14_enum_payload
    16_slice_in
)

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

echo "== S5 checkpoint: the rest of the language (probes 09-14, 16) =="

failures=0
for p in "${probes[@]}"; do
    name="$(basename "$p")"
    src="$root/test/translation-probes/$p.lox"
    if [ ! -f "$src" ]; then
        echo "  FAIL  $name (no such probe: $src)"
        failures=$((failures + 1))
        continue
    fi

    if ! "$qbe_emit_program" "$src" >"$name.ssa" 2>"$name.descs"; then
        echo "  FAIL  $name (qbe_emit_program)"
        sed 's/^/          /' "$name.descs"
        failures=$((failures + 1))
        continue
    fi

    # Same per-probe harness generation as S4's own checkpoint script: one
    # extern decl + one RtFunctionDesc entry per "id arity hash symbol"
    # line qbe_emit_program wrote to stderr.
    harness_cpp="${name}_harness.cpp"
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
        done <"$name.descs"
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
        done <"$name.descs"
        echo '    };'
        echo '    Runtime* rt = rt_startup(source.c_str(), descs, sizeof(descs) / sizeof(descs[0]), true);'
        echo '    if (rt == nullptr) { return 65; }'
        echo '    int status = rt_call(rt, 0);'
        echo '    rt_shutdown(rt);'
        echo '    return status == 0 ? 0 : 70;'
        echo '}'
    } >"$harness_cpp"

    if ! qbe -o "$name.s" "$name.ssa" 2>"$name.qbe_err"; then
        echo "  FAIL  $name (qbe)"
        sed 's/^/          /' "$name.qbe_err"
        failures=$((failures + 1))
        continue
    fi

    if ! c++ -std=c++20 -I "$root/src" "$harness_cpp" "$name.s" "$libloxrt" \
             -lstdc++ -lm -o "${name}_bin" 2>"$name.cc_err"; then
        echo "  FAIL  $name (cc)"
        sed 's/^/          /' "$name.cc_err"
        failures=$((failures + 1))
        continue
    fi

    qbe_out="$(./"${name}_bin" "$src" 2>"$name.qbe_stderr")" || qbe_status=$?
    qbe_status="${qbe_status:-0}"
    native_out="$("$loxpp_bin" "$src" 2>"$name.native_stderr")" || native_status=$?
    native_status="${native_status:-0}"

    if [ "$qbe_out" = "$native_out" ] && [ "$qbe_status" = "$native_status" ]; then
        printf '  ok    %s\n' "$name"
    else
        printf '  FAIL  %s\n' "$name"
        printf '          qbe    (exit %s): %s\n' "$qbe_status" "$qbe_out"
        printf '          native (exit %s): %s\n' "$native_status" "$native_out"
        failures=$((failures + 1))
    fi
    unset qbe_status native_status
done

echo
if [ "$failures" -ne 0 ]; then
    echo "$failures S5 checkpoint check(s) failed."
    exit 1
fi
echo "S5 checkpoint passed."
