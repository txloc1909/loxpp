#!/bin/bash
# Wrapper to run a Lox source file through lox_interpreter.lox
# Usage: lox_wrapper.sh <source.lox>
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOXPP="${SCRIPT_DIR}/../build/loxpp"
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
done < <(cat "$1" | "$LOXPP" "${SCRIPT_DIR}/lox_interpreter.lox")
exit $exitcode
