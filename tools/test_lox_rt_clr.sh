#!/usr/bin/env bash
#
# Builds the CLR runtime library, then runs its whole test suite - one
# `dotnet run` process; no test framework in the build container, so
# runtime/clr/test/Program.cs discovers and runs every `*Test.Run()` suite
# itself and returns their combined exit code.
#
# Includes the %g differential parity test (StringifyDifferentialTest),
# which needs an already-built native build/loxpp binary as its oracle.
# Build it first with `cmake --preset release && cmake --build build
# --target loxpp` - the release preset, not debug: the debug preset turns
# on LOXPP_DEBUG_PRINT_CODE and LOXPP_DEBUG_TRACE_EXECUTION by default,
# which put disassembly and trace text on that same binary's stdout
# alongside its `print` output. The test itself proves the binary fit for
# this before trusting it, and fails loudly rather than silently comparing
# against the wrong line when it is not. This script fails loudly rather
# than skip the whole check silently when the binary is missing at all,
# mirroring tools/check_jvm_probes.sh's own precondition.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_project="$root/runtime/clr/test/LoxRuntimeTests.csproj"

native_bin="${LOXPP_BIN:-$root/build/loxpp}"
if [ ! -x "$native_bin" ]; then
    echo "test_lox_rt_clr.sh: no loxpp binary at $native_bin" >&2
    echo "test_lox_rt_clr.sh: build it first (cmake --preset release && cmake --build build --target loxpp)" >&2
    exit 1
fi

"$root/tools/build_lox_rt_clr.sh"

export LOXPP_BIN="$native_bin"
dotnet run --project "$test_project" -c Release --no-launch-profile
