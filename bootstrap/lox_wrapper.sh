#!/bin/bash
# Wrapper to run a Lox/Lox++ source file through the appropriate bootstrap interpreter.
# Usage: lox_wrapper.sh <source.lox>
# LANGUAGE=LOX (default) uses lox_interpreter.lox
# LANGUAGE=LOXPP        uses loxpp_interpreter.lox
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOXPP="${SCRIPT_DIR}/../build/loxpp"

case "${LANGUAGE:-LOX}" in
    LOXPP)
        INTERPRETER="${SCRIPT_DIR}/loxpp_interpreter.lox"
        # Sentinel protocol: wrap source so that subsequent input() calls in
        # the interpreted program can still read from the caller's stdin.
        FEED_CMD='{ printf "__SOURCE_BEGIN__\n"; cat "$1"; printf "__SOURCE_END__\n"; cat; }'
        ;;
    *)
        INTERPRETER="${SCRIPT_DIR}/lox_interpreter.lox"
        FEED_CMD='cat "$1"'
        ;;
esac

exitcode=0
while IFS= read -r line; do
    case "$line" in
        LOXERR65\ *)
            printf '%s\n' "${line#LOXERR65 }" >&2
            exitcode=65
            ;;
        LOXERR70\ *)
            printf '%s\n' "${line#LOXERR70 }" >&2
            exitcode=70
            ;;
        *)
            printf '%s\n' "$line"
            ;;
    esac
done < <(eval "$FEED_CMD" | "$LOXPP" "$INTERPRETER")
exit $exitcode
