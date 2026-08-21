#!/usr/bin/env bash
#
# Builds the CLR runtime library with plain `dotnet build`. runtime/clr's
# project is a dependency-free net8.0 class library (see
# notes/backend-implementation-dag.md: the dev-managed image has no maven
# and no gradle, and no network at test time), so the restore `dotnet
# build` performs first needs no network access either.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_dir="$root/runtime/clr"
build_dir="$runtime_dir/bin/Release/net8.0"
dll_path="$runtime_dir/LoxRuntime.dll"

dotnet build "$runtime_dir/LoxRuntime.csproj" -c Release --nologo

# Copy to a stable, canonical path (mirroring runtime/jvm/lox-rt.jar) so
# every other tool that links against this runtime has one fixed location
# to reference, independent of dotnet's own configuration/TFM-named output
# directory.
cp "$build_dir/LoxRuntime.dll" "$dll_path"

echo "Built $dll_path"
