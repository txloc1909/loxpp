#!/bin/sh
# your_program.sh — Codecrafters entrypoint for the Lox++ Redis server.
#
# redis.lox is pure Lox++: the RESP parser, the keyspace, and every command
# run inside the VM. This wrapper only resolves the loxpp binary.
#
# Binary resolution: build/loxpp next to this repo checkout first,
# falling back to loxpp on PATH.
set -u

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/../../.." && pwd)
SCRIPT="$HERE/redis.lox"

LOXPP="$ROOT/build/loxpp"
if [ ! -x "$LOXPP" ]; then
    LOXPP=$(command -v loxpp 2>/dev/null || true)
fi
if [ -z "${LOXPP:-}" ] || [ ! -x "$LOXPP" ]; then
    echo "your_program.sh: loxpp binary not found (tried $ROOT/build/loxpp and PATH)" >&2
    exit 74
fi

exec "$LOXPP" "$SCRIPT" "$@"
