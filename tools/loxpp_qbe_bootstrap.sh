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
# status file written inside the pipeline's left side (its subshell exit
# status is not visible to the parent shell any other way) so a host-level
# fault still fails the wrapper instead of silently reporting exit 0. This is
# the same fallback bootstrap/lox_wrapper.sh needs for its own native host.
#
# sed filters the stream because it passes NUL bytes through and bash `read`
# drops them: a program that prints a NUL byte must reach stdout intact.
# LC_ALL=C keeps the filter byte-oriented. Each protocol line goes to stderr
# without its prefix, and its exit code is appended to $code_file, so the
# last protocol line decides the exit code (same filter as
# bootstrap/lox_wrapper.sh).
status_file="$(mktemp)"
code_file="$(mktemp)"
trap 'rm -f "$status_file" "$code_file"' EXIT
{ eval "$FEED_CMD" | "$LOXPP_QBE" "$INTERPRETER"; echo "$?" > "$status_file"; } \
    | LC_ALL=C sed -u \
        -e '/^LOXERR65 /{s///;w /dev/stderr' -e 's/.*/65/;w '"$code_file" -e 'd;}' \
        -e '/^LOXERR70 /{s///;w /dev/stderr' -e 's/.*/70/;w '"$code_file" -e 'd;}'
last_code="$(tail -n 1 "$code_file")"
if [ -n "$last_code" ]; then
    exitcode="$last_code"
fi
qbe_status="$(cat "$status_file" 2>/dev/null)"
if [ "$exitcode" -eq 0 ] && [ -n "$qbe_status" ] && [ "$qbe_status" -ne 0 ]; then
    exitcode="$qbe_status"
fi
exit $exitcode
