#!/bin/bash
# Wrapper to run a Lox/Lox++ source file through the appropriate bootstrap interpreter.
# Usage: lox_wrapper.sh <source.lox> [args...]
# LANGUAGE=LOX (default) uses lox_interpreter.lox
# LANGUAGE=LOXPP        uses loxpp_interpreter.lox
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOXPP="${SCRIPT_DIR}/../build/loxpp"
# Program arguments pass through to the host, so the interpreted args()
# (which delegates to the host args()) sees the program's arguments.
SRC="$1"; shift

case "${LANGUAGE:-LOX}" in
    LOXPP)
        INTERPRETER="${SCRIPT_DIR}/loxpp_interpreter.lox"
        # Sentinel protocol: wrap source so that subsequent input() calls in
        # the interpreted program can still read from the caller's stdin.
        # If $SRC's last line has no trailing newline, "cat" and the
        # __SOURCE_END__ printf below would glue onto the same line,
        # corrupting the interpreted program's last line with the sentinel
        # text (issue #328). Insert one only when $SRC itself needs it:
        # tail -c 1 prints the file's last byte, and command substitution
        # strips a trailing newline from its own output, so the test comes
        # back empty exactly when that last byte already was "\n".
        FEED_CMD='{ printf "__SOURCE_BEGIN__\n"; cat "$SRC"; [ -s "$SRC" ] && [ -n "$(tail -c 1 "$SRC")" ] && printf "\n"; printf "__SOURCE_END__\n"; cat; }'
        ;;
    *)
        INTERPRETER="${SCRIPT_DIR}/lox_interpreter.lox"
        FEED_CMD='cat "$SRC"'
        ;;
esac

exitcode=0
# The bootstrap interpreter reports its own controlled halts (compile error,
# uncaught throw, ...) via a LOXERR65/LOXERR70-prefixed stdout line, parsed
# below. But a fault one layer below that protocol -- the native VM crashing
# on its own interpretation of the bootstrap script, e.g. an arity mismatch
# inside loxpp_interpreter.lox -- never prints such a line; it only shows up
# as $LOXPP's own nonzero exit status. Capture that status via a status file
# written inside the process substitution (its subshell exit status isn't
# visible to the parent shell any other way) so a native-level crash still
# fails the wrapper instead of silently reporting exit 0.
status_file="$(mktemp)"
trap 'rm -f "$status_file"' EXIT
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
done < <(eval "$FEED_CMD" | "$LOXPP" "$INTERPRETER" "$@"; echo "$?" > "$status_file")
loxpp_status="$(cat "$status_file" 2>/dev/null)"
if [ "$exitcode" -eq 0 ] && [ -n "$loxpp_status" ] && [ "$loxpp_status" -ne 0 ]; then
    exitcode="$loxpp_status"
fi
exit $exitcode
