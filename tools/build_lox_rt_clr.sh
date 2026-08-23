#!/usr/bin/env bash
#
# Builds the CLR runtime library and its run-harness host with plain
# `dotnet build`. Both are dependency-free net8.0 projects (see
# notes/backend-implementation-dag.md: the dev-managed image has no maven
# and no gradle, and no network at test time), so the restore `dotnet
# build` performs first needs no network access either.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_dir="$root/runtime/clr"
rt_build_dir="$runtime_dir/bin/Release/net8.0"
rt_dll_path="$runtime_dir/LoxRuntime.dll"

dotnet build "$runtime_dir/LoxRuntime.csproj" -c Release --nologo

# Copy to a stable, canonical path (mirroring runtime/jvm/lox-rt.jar) so
# every other tool that links against this runtime has one fixed location
# to reference, independent of dotnet's own configuration/TFM-named output
# directory.
cp "$rt_build_dir/LoxRuntime.dll" "$rt_dll_path"

echo "Built $rt_dll_path"

host_dir="$runtime_dir/host"
host_build_dir="$host_dir/bin/Release/net8.0"

dotnet build "$host_dir/LoxHost.csproj" -c Release --nologo

# tools/clr_run.sh runs every emitted program through this host (see
# LoxHost.cs), so it needs the same canonical-path treatment as the runtime
# library: LoxHost.dll plus the runtimeconfig.json `dotnet build` generates
# for it (ilasm-produced assemblies need one hand-written, per
# tools/clr_run.sh's own note; a `dotnet build` Exe project already emits
# its own).
for f in LoxHost.dll LoxHost.runtimeconfig.json; do
    cp "$host_build_dir/$f" "$runtime_dir/$f"
done

echo "Built $runtime_dir/LoxHost.dll"
