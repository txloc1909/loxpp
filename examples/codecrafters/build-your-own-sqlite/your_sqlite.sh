#!/bin/sh
# your_sqlite.sh — Codecrafters entrypoint for the Lox++ SQLite reader.
#
# sqlite.lox is pure Lox++. The one thing outside its reach is the 256-byte
# lookup table its ord()/chr() are built from (see the comment atop
# sqlite.lox for why): this wrapper points LOXPP_BYTE_TABLE at the
# committed bytetable.bin next to this script.
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

LOXPP_BYTE_TABLE="$HERE/bytetable.bin"
export LOXPP_BYTE_TABLE

exec "$LOXPP" "$SCRIPT" "$@"
