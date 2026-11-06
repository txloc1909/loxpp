#!/bin/sh
# your_program.sh — Codecrafters entrypoint for the Lox++ Kafka broker.
#
# kafka.lox is pure Lox++: the wire protocol, the `__cluster_metadata` reader,
# and the partition log files all run inside the VM. This wrapper only
# resolves the loxpp binary.
#
# The challenge runs it as: ./your_program.sh /tmp/server.properties
#
# Binary resolution: build/loxpp next to this repo checkout first,
# falling back to loxpp on PATH.
set -u

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/../../.." && pwd)
SCRIPT="$HERE/kafka.lox"

LOXPP="$ROOT/build/loxpp"
if [ ! -x "$LOXPP" ]; then
    LOXPP=$(command -v loxpp 2>/dev/null || true)
fi
if [ -z "${LOXPP:-}" ] || [ ! -x "$LOXPP" ]; then
    echo "your_program.sh: loxpp binary not found (tried $ROOT/build/loxpp and PATH)" >&2
    exit 74
fi

exec "$LOXPP" "$SCRIPT" "$@"
