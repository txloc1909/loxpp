#!/bin/sh
# your_sqlite.sh — Codecrafters entrypoint for the Lox++ SQLite reader.
#
# sqlite.lox is pure Lox++: it reads the database with ord()/chr() and
# compares index keys with the built-in String order.
#
# Binary resolution: build/loxpp next to this repo checkout first,
# falling back to loxpp on PATH.
set -u

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/../../.." && pwd)
SCRIPT="$HERE/sqlite.lox"

LOXPP="$ROOT/build/loxpp"
if [ ! -x "$LOXPP" ]; then
    LOXPP=$(command -v loxpp 2>/dev/null || true)
fi
if [ -z "${LOXPP:-}" ] || [ ! -x "$LOXPP" ]; then
    echo "your_sqlite.sh: loxpp binary not found (tried $ROOT/build/loxpp and PATH)" >&2
    exit 74
fi

exec "$LOXPP" "$SCRIPT" "$@"
