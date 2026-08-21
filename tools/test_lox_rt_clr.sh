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
#
# LOXPP_EMPTY_PROBE is exported here, empty, on purpose: env() must read
# it as the empty string, not nil, because the variable IS defined (see
# env()'s test in RuntimeStdlibTest.cs and StdlibDifferentialTest.cs).
# `Environment.SetEnvironmentVariable(name, "")` cannot stand in for this
# fixture - .NET documents that call as deleting the variable, so a test
# that set it that way would observe nil instead and prove nothing about
# env()'s real behaviour. Only a variable the OS environment block itself
# carries empty, as this export gives every process below, exercises it.
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
export LOXPP_EMPTY_PROBE=

# `dotnet run` launches the built test binary through its own extra process
# hop, which was measured to drop an empty-valued (but exported) variable
# such as LOXPP_EMPTY_PROBE before the test process ever sees it - even
# though the exact same binary, run directly, inherits it correctly.
# Building then running the DLL directly avoids that hop.
dotnet build "$test_project" -c Release
dotnet "$root/runtime/clr/test/bin/Release/net8.0/LoxRuntimeTests.dll"
