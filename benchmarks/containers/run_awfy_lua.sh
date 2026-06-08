#!/bin/sh
# Run a single AWFY Lua benchmark via the harness.
# Usage: run_awfy_lua.sh <benchmark-file.lua>
#
# The harness.lua takes a class name (PascalCase) and must be run from
# the benchmarks directory so Lua can find the som/benchmark modules.
file="$1"
base=$(basename "$file" .lua)

# Class name overrides for non-trivial cases
case "$base" in
  nbody)     name=NBody ;;
  deltablue) name=DeltaBlue ;;
  cd)        name=CD ;;
  json)      name=Json ;;
  *)         name=$(echo "$base" | awk '{print toupper(substr($0,1,1)) substr($0,2)}') ;;
esac

cd /benchmarks/awfy
exec lua5.4 /benchmarks/awfy/harness.lua "$name" 5
