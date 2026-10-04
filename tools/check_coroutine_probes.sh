#!/usr/bin/env bash
#
# Research gate: mission #523 node #525's coroutine probe corpus.
#
# Runs test/coroutine-probes/*.lox through the native interpreter and checks
# each program's // CHECK: directives (tools/check_examples.py's
# FileCheck-style harness) against its actual stdout.
#
# This corpus pins the target semantics of the coroutine primitive before any
# backend implements it: the probes use `yield`, `coroutine.create`,
# `Coroutine.resume`/`status`, and for-in over a Coroutine. None of those
# exist yet, so every probe FAILS (parse error on `yield`, or a runtime
# UndefinedVariableError on `coroutine`) at this node. That all-red run is
# the recorded "the check can fail" proof. The corpus turns green as the
# native primitive (#526) and the suspendable-iterator node (#527) land.
#
# Requires a build/loxpp binary. Use the release preset: the debug preset
# leaves LOXPP_DEBUG_TRACE_EXECUTION/LOXPP_DEBUG_PRINT_CODE on, which floods
# stdout with a per-instruction trace (see AGENTS.md).
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
loxpp="${LOXPP_BIN:-$root/build/loxpp}"

if [ ! -x "$loxpp" ]; then
    echo "check_coroutine_probes.sh: no loxpp binary at $loxpp" >&2
    exit 1
fi

exec python3 "$root/tools/check_examples.py" "$loxpp" \
    "$root/test/coroutine-probes/"
