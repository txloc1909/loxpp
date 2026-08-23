#!/bin/bash
# CLR twin of tools/loxpp_jvm_bootstrap.sh.
# Runs a Lox/Lox++ source file through the self-hosted interpreter, the same
# way lox_wrapper.sh does, but executes the interpreter itself on the CLR
# backend (tools/loxpp_clr.sh) instead of the native build/loxpp binary.
#
# Usage: loxpp_clr_bootstrap.sh <source.lox>
# LANGUAGE=LOX (default) uses lox_interpreter.lox
# LANGUAGE=LOXPP        uses loxpp_interpreter.lox
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
LOXPP_CLR="$SCRIPT_DIR/loxpp_clr.sh"

case "${LANGUAGE:-LOX}" in
    LOXPP)
        INTERPRETER="${ROOT_DIR}/bootstrap/loxpp_interpreter.lox"
        # Sentinel protocol: wrap source so that subsequent input() calls in
        # the interpreted program can still read from the caller's stdin.
        FEED_CMD='{ printf "__SOURCE_BEGIN__\n"; cat "$1"; printf "__SOURCE_END__\n"; cat; }'
        ;;
    *)
        INTERPRETER="${ROOT_DIR}/bootstrap/lox_interpreter.lox"
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
done < <(eval "$FEED_CMD" | "$LOXPP_CLR" "$INTERPRETER")
exit $exitcode
