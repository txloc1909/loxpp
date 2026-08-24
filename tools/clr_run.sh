#!/usr/bin/env bash
#
# Assembles a directory of CIL (.il) files into one managed executable, then
# runs it against the CLR runtime library. This is the fixed harness a code
# generator plugs into: it does not know or care how the .il files were
# produced, so any emitter code can call it unchanged.
#
# Usage: tools/clr_run.sh <il-dir> <rt-dll> <main-class> [arg...]
#
#   <il-dir>     directory holding one or more *.il files. ilasm assembles
#                every one of them together into a single output assembly
#                (verified: ilasm accepts more than one source file and
#                links them into one PE, so more than one emitted class can
#                share this one assembly — proven with a two-file, two-class
#                probe against ilasm 8.0.0 in this project's dev-managed
#                image). Give a scratch directory: this harness writes
#                <il-dir>/<main-class>.dll.
#   <rt-dll>     path to runtime/clr/LoxRuntime.dll.
#   <main-class> name of the class whose static Main(string[] args) is the
#                entry point — the same name the emitted *.il file's own
#                `.assembly`/`.module` directives use (loxpp --target clr
#                names it "LoxMain", matching --target jvm's "LoxMain").
#   [arg...]     program arguments. LoxHost (runtime/clr/host/LoxHost.cs)
#                binds these to the entry point's own `args` parameter, and
#                the emitted prologue forwards that array to
#                LoxRuntime.SetProgramArgs before the script body runs, so
#                the native `args()` global answers them.
#
# The assembled program does not run as its own process. It runs through
# LoxHost, on a thread LoxHost sizes itself (LOX_RT_CLR_HOST_DLL overrides
# its path; LOX_CLR_STACK_BYTES overrides the stack size, default 256 MiB) —
# see LoxHost.cs for why every CLR run goes through this indirection, not
# only the self-hosted interpreter that first needed it.
#
# stdin, stdout, stderr, and the exit code all pass through to and from
# dotnet unchanged, because later checks diff this output against
# build/loxpp.
set -euo pipefail

if [ "$#" -lt 3 ]; then
    echo "usage: tools/clr_run.sh <il-dir> <rt-dll> <main-class> [arg...]" >&2
    exit 2
fi

il_dir="$1"
rt_dll="$2"
main_class="$3"
program_args=("${@:4}")

# Sibling of rt_dll by default: tools/build_lox_rt_clr.sh writes both into
# the same runtime/clr/ directory.
host_dll="${LOX_RT_CLR_HOST_DLL:-$(dirname "$rt_dll")/LoxHost.dll}"
stack_bytes="${LOX_CLR_STACK_BYTES:-268435456}"

if [ ! -d "$il_dir" ]; then
    echo "clr_run.sh: no such directory: $il_dir" >&2
    exit 1
fi

if [ ! -f "$rt_dll" ]; then
    echo "clr_run.sh: no such runtime dll: $rt_dll" >&2
    echo "clr_run.sh: run tools/build_lox_rt_clr.sh first." >&2
    exit 1
fi

if [ ! -f "$host_dll" ]; then
    echo "clr_run.sh: no such host dll: $host_dll" >&2
    echo "clr_run.sh: run tools/build_lox_rt_clr.sh first (writes both dlls" >&2
    echo "clr_run.sh: into runtime/clr/), or set LOX_RT_CLR_HOST_DLL if" >&2
    echo "clr_run.sh: LOX_RT_CLR_DLL points at a copy built elsewhere." >&2
    exit 1
fi

il_files=("$il_dir"/*.il)
if [ ! -e "${il_files[0]}" ]; then
    echo "clr_run.sh: no .il files in $il_dir" >&2
    exit 1
fi

output_dll="$il_dir/$main_class.dll"
rm -f "$output_dll"

# ilasm's own exit code and "Operation completed successfully" banner are
# not proof of anything beyond "ilasm did not crash" — the real proof is
# the file-existence check below, so run it and keep its output only to
# show the caller, on failure, not to judge success by (mirrors
# tools/jvm_run.sh's own reasoning about jasmin's exit code).
ilasm_out="$(ilasm -exe -output:"$output_dll" "${il_files[@]}" 2>&1)" || true

if [ ! -f "$output_dll" ]; then
    echo "clr_run.sh: ilasm assembly failed" >&2
    printf '%s\n' "$ilasm_out" >&2
    exit 1
fi

# ilasm emits no runtimeconfig.json, and dotnet refuses to run an assembly
# with no such file for its own entry assembly (notes/backend-implementation-
# dag.md) — but $output_dll is no longer dotnet's own entry assembly, only a
# dependency LoxHost loads by reflection, so it needs none of its own; only
# LoxHost.dll does, and `dotnet build` already wrote that one
# (tools/build_lox_rt_clr.sh).

# LoxHost loads $output_dll with Assembly.LoadFrom, which also probes that
# assembly's own directory for its dependencies — so the runtime dll needs a
# copy right there too, not only next to LoxHost.dll (tools/build_lox_rt_clr.sh
# already puts one there) or on ilasm's own assemble-time search path.
cp "$rt_dll" "$il_dir/"

# exec, not a captured call: it replaces this script with dotnet, so stdin,
# stdout, stderr, and the exit code are dotnet's own, not a copy.
exec dotnet "$host_dll" "$stack_bytes" "$output_dll" "${program_args[@]}"
