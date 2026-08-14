#!/usr/bin/env bash
#
# Builds the JVM runtime library with plain javac + jar. The dev-managed image
# has no maven and no gradle (see notes/backend-implementation-dag.md), so the
# build is two direct toolchain calls and nothing else.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_dir="$root/runtime/jvm"
classes_dir="$runtime_dir/classes"
jar_path="$runtime_dir/lox-rt.jar"

rm -rf "$classes_dir"
mkdir -p "$classes_dir"

javac -d "$classes_dir" "$runtime_dir"/src/lox/*.java
jar --create --file "$jar_path" -C "$classes_dir" .

echo "Built $jar_path"
