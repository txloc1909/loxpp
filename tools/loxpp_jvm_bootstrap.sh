#!/bin/bash
# JVM twin of bootstrap/lox_wrapper.sh.
# Runs a Lox/Lox++ source file through the self-hosted interpreter, the same
# way lox_wrapper.sh does, but executes the interpreter itself on the JVM
# backend (tools/loxpp_jvm.sh) instead of the native build/loxpp binary.
#
# Usage: loxpp_jvm_bootstrap.sh <source.lox> [args...]
# LANGUAGE=LOX (default) uses lox_interpreter.lox
# LANGUAGE=LOXPP        uses loxpp_interpreter.lox
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
LOXPP_JVM="$SCRIPT_DIR/loxpp_jvm.sh"
# Program arguments pass through to the host, so the interpreted args()
# (which delegates to the host args()) sees the program's arguments.
SRC="$1"; shift

case "${LANGUAGE:-LOX}" in
    LOXPP)
        INTERPRETER="${ROOT_DIR}/bootstrap/loxpp_interpreter.lox"
        # Sentinel protocol: wrap source so that subsequent input() calls in
        # the interpreted program can still read from the caller's stdin.
        # If $SRC's last line has no trailing newline, "cat" and the
        # __SOURCE_END__ printf below would glue onto the same line,
        # corrupting the interpreted program's last line with the sentinel
        # text (issue #328, same fix as bootstrap/lox_wrapper.sh). Insert
        # one only when $SRC itself needs it: tail -c 1 prints the file's
        # last byte, and command substitution strips a trailing newline
        # from its own output, so the test comes back empty exactly when
        # that last byte already was "\n".
        FEED_CMD='{ printf "__SOURCE_BEGIN__\n"; cat "$SRC"; [ -s "$SRC" ] && [ -n "$(tail -c 1 "$SRC")" ] && printf "\n"; printf "__SOURCE_END__\n"; cat; }'
        ;;
    *)
        INTERPRETER="${ROOT_DIR}/bootstrap/lox_interpreter.lox"
        FEED_CMD='cat "$SRC"'
        ;;
esac

exitcode=0
# sed filters the stream because it passes NUL bytes through and bash `read`
# drops them: a program that prints a NUL byte must reach stdout intact.
# LC_ALL=C keeps the filter byte-oriented. Each protocol line goes to stderr
# without its prefix, and its exit code is appended to $code_file, so the
# last protocol line decides the exit code (same filter as
# bootstrap/lox_wrapper.sh).
code_file="$(mktemp)"
trap 'rm -f "$code_file"' EXIT
eval "$FEED_CMD" | "$LOXPP_JVM" "$INTERPRETER" "$@" \
    | LC_ALL=C sed -u \
        -e '/^LOXERR65 /{s///;w /dev/stderr' -e 's/.*/65/;w '"$code_file" -e 'd;}' \
        -e '/^LOXERR70 /{s///;w /dev/stderr' -e 's/.*/70/;w '"$code_file" -e 'd;}'
last_code="$(tail -n 1 "$code_file")"
if [ -n "$last_code" ]; then
    exitcode="$last_code"
fi
exit $exitcode
