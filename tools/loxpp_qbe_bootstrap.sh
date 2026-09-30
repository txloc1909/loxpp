#!/usr/bin/env bash
#
# QBE twin of bootstrap/lox_wrapper.sh, matching tools/loxpp_jvm_bootstrap.sh.
# Runs a Lox/Lox++ source file through the
# self-hosted interpreter, the same way lox_wrapper.sh does, but executes
# the interpreter itself on the QBE backend (tools/loxpp_qbe.sh) instead of
# the native build/loxpp binary — so a diff between this script and
# lox_wrapper.sh proves the QBE backend on a large, real program (issue
# #460's own Q8 hazard: "measure QBE's time and memory on
# bootstrap/loxpp_interpreter.lox early").
#
# Usage: loxpp_qbe_bootstrap.sh <source.lox> [arg...]
# LANGUAGE=LOX (default) uses lox_interpreter.lox — this backend does not
# interpret plain Lox this way; only LANGUAGE=LOXPP compiles anything, since
# lox_interpreter.lox is itself plain Lox++ source either way. The LOX case
# is kept only so this script's usage matches its JVM twin exactly.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
LOXPP_QBE="$SCRIPT_DIR/loxpp_qbe.sh"
SRC="$1"; shift

case "${LANGUAGE:-LOX}" in
    LOXPP)
        INTERPRETER="${ROOT_DIR}/bootstrap/loxpp_interpreter.lox"
        ;;
    *)
        INTERPRETER="${ROOT_DIR}/bootstrap/lox_interpreter.lox"
        ;;
esac

# Sentinel protocol (issue #328, same fix as bootstrap/lox_wrapper.sh): a
# missing trailing newline on $SRC would otherwise glue onto
# __SOURCE_END__, corrupting the interpreted program's own last line.
FEED_CMD='{ printf "__SOURCE_BEGIN__\n"; cat "$SRC"; [ -s "$SRC" ] && [ -n "$(tail -c 1 "$SRC")" ] && printf "\n"; printf "__SOURCE_END__\n"; cat; }'

exitcode=0
# A fault one layer below the LOXERR protocol above -- the QBE-compiled
# interpreter itself failing on a runtime error inside loxpp_interpreter.lox
# that the interpreter never catches -- prints no LOXERR line; it only shows
# up as loxpp_qbe.sh's own nonzero exit status. Capture that status via a
# status file written inside the process substitution (its subshell exit
# status is not visible to the parent shell any other way) so a host-level
# fault still fails the wrapper instead of silently reporting exit 0. This is
# the same fallback bootstrap/lox_wrapper.sh needs for its own native host.
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
done < <(eval "$FEED_CMD" | "$LOXPP_QBE" "$INTERPRETER"; echo "$?" > "$status_file")
qbe_status="$(cat "$status_file" 2>/dev/null)"
if [ "$exitcode" -eq 0 ] && [ -n "$qbe_status" ] && [ "$qbe_status" -ne 0 ]; then
    exitcode="$qbe_status"
fi
exit $exitcode
