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
# `Coroutine.resume`/`status`, and for-in over a Coroutine. The recorded
# all-red run (every probe failed before the primitive existed) is the "the
# check can fail" proof. The native primitive makes every probe but the
# for-in one pass; the suspendable-iterator node makes that one pass too and
# removes it from xfail.txt.
#
# test/coroutine-probes/xfail.txt lists probes expected to fail until a later
# node lands. They are excluded from the passing run and asserted to still
# fail, so the gate is green while still tracking the gap. If an xfail probe
# starts passing, the gate fails, which tells the fixing node to delete its
# line.
#
# Requires a build/loxpp binary. Use the release preset: the debug preset
# leaves LOXPP_DEBUG_TRACE_EXECUTION/LOXPP_DEBUG_PRINT_CODE on, which floods
# stdout with a per-instruction trace (see AGENTS.md).
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
loxpp="${LOXPP_BIN:-$root/build/loxpp}"
probes_dir="$root/test/coroutine-probes"
xfail_file="$probes_dir/xfail.txt"

if [ ! -x "$loxpp" ]; then
    echo "check_coroutine_probes.sh: no loxpp binary at $loxpp" >&2
    exit 1
fi

status=0
python3 "$root/tools/check_examples.py" "$loxpp" "$probes_dir/" \
    --exclude "$xfail_file" || status=1

while read -r name reason; do
    case "$name" in '' | '#'*) continue ;; esac
    if "$loxpp" "$probes_dir/$name" >/dev/null 2>&1; then
        echo "FAIL  $name  (expected to fail until fixed: $reason)"
        status=1
    else
        echo "XFAIL $name  ($reason)"
    fi
done <"$xfail_file"

exit "$status"
