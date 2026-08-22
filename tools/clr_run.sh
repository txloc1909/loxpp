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
#                <il-dir>/<main-class>.dll and its runtimeconfig.json.
#   <rt-dll>     path to runtime/clr/LoxRuntime.dll.
#   <main-class> name of the class whose static Main(string[] args) is the
#                entry point — the same name the emitted *.il file's own
#                `.assembly`/`.module` directives use (loxpp --target clr
#                names it "LoxMain", matching --target jvm's "LoxMain").
#   [arg...]     program arguments. dotnet binds these to Main's own
#                `args` parameter, and the emitted prologue forwards that
#                array to LoxRuntime.SetProgramArgs before the script body
#                runs, so the native `args()` global answers them.
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

if [ ! -d "$il_dir" ]; then
    echo "clr_run.sh: no such directory: $il_dir" >&2
    exit 1
fi

if [ ! -f "$rt_dll" ]; then
    echo "clr_run.sh: no such runtime dll: $rt_dll" >&2
    echo "clr_run.sh: run tools/build_lox_rt_clr.sh first." >&2
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

runtime_version="$(dotnet --list-runtimes 2>/dev/null \
    | awk '$1 == "Microsoft.NETCore.App" { print $2 }' | sort -V | tail -1)"
if [ -z "$runtime_version" ]; then
    echo "clr_run.sh: no Microsoft.NETCore.App runtime found (dotnet --list-runtimes)" >&2
    exit 1
fi

# ilasm emits no runtimeconfig.json, and dotnet refuses to run an assembly
# without one (notes/backend-implementation-dag.md). The tfm here is
# derived from the discovered runtime, not hardcoded, so a runtime upgrade
# in the image cannot silently pair a stale tfm with a newer framework
# version (mirrors tools/check_managed_toolchains.sh's own derivation).
cat > "$il_dir/$main_class.runtimeconfig.json" <<JSON
{
  "runtimeOptions": {
    "tfm": "net${runtime_version%%.*}.0",
    "framework": {
      "name": "Microsoft.NETCore.App",
      "version": "${runtime_version}"
    }
  }
}
JSON

# dotnet resolves an `.assembly extern LoxRuntime {}` reference by probing
# next to the running assembly, so the runtime dll needs a copy right there
# too, not only on ilasm's own assemble-time search path.
cp "$rt_dll" "$il_dir/"

# exec, not a captured call: it replaces this script with dotnet, so stdin,
# stdout, stderr, and the exit code are dotnet's own, not a copy.
exec dotnet "$output_dll" "${program_args[@]}"
