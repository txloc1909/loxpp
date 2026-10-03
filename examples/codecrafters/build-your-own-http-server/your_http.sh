#!/bin/sh
# your_http.sh — Codecrafters entrypoint for the Lox++ HTTP server.
#
# http.lox is pure Lox++: the HTTP/1.1 parser, router, and gzip encoder
# (DEFLATE + CRC32) all run inside the VM. This wrapper only resolves the
# loxpp binary.
#
# Binary resolution: build/loxpp next to this repo checkout first,
# falling back to loxpp on PATH.
set -u

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/../../.." && pwd)
SCRIPT="$HERE/http.lox"

LOXPP="$ROOT/build/loxpp"
if [ ! -x "$LOXPP" ]; then
    LOXPP=$(command -v loxpp 2>/dev/null || true)
fi
if [ -z "${LOXPP:-}" ] || [ ! -x "$LOXPP" ]; then
    echo "your_http.sh: loxpp binary not found (tried $ROOT/build/loxpp and PATH)" >&2
    exit 74
fi

exec "$LOXPP" "$SCRIPT" "$@"
