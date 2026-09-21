#!/bin/sh
# your_grep.sh — Codecrafters entrypoint for the Lox++ grep.
#
# The regex engine itself is pure Lox++ (grep.lox). This wrapper owns the
# two things Lox++ cannot do:
#   - recursive directory expansion for -r (no readdir in the stdlib);
#   - --color=auto TTY detection (no isatty in the stdlib) plus the ESC
#     bytes for highlighting (ESC is not writable as a Lox++ literal),
#     passed via LOXPP_COLOR_OPEN / LOXPP_COLOR_CLOSE.
#
# Binary resolution: build/loxpp next to this repo checkout first,
# falling back to loxpp on PATH.
set -u

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/../../.." && pwd)
SCRIPT="$HERE/grep.lox"

LOXPP="$ROOT/build/loxpp"
if [ ! -x "$LOXPP" ]; then
    LOXPP=$(command -v loxpp 2>/dev/null || true)
fi
if [ -z "${LOXPP:-}" ] || [ ! -x "$LOXPP" ]; then
    echo "your_grep.sh: loxpp binary not found (tried $ROOT/build/loxpp and PATH)" >&2
    exit 74
fi

# Pass 1: resolve --color=auto against stdout's TTY-ness, preserving
# every other argument byte-for-byte (patterns may contain spaces).
HAD_R=0
for a in "$@"; do
    if [ "$a" = "-r" ]; then
        HAD_R=1
        break
    fi
done

left=$#
while [ "$left" -gt 0 ]; do
    case "$1" in
        --color=auto)
            if [ -t 1 ]; then
                set -- "$@" "--color=always"
            else
                set -- "$@" "--color=never"
            fi
            shift
            left=$((left - 1))
            ;;
        --color)
            if [ "$left" -ge 2 ] && [ "${2:-}" = "auto" ]; then
                if [ -t 1 ]; then
                    CV="always"
                else
                    CV="never"
                fi
                set -- "$@" "--color" "--color=$CV"
                shift
                shift
                left=$((left - 2))
            else
                set -- "$@" "$1"
                shift
                left=$((left - 1))
            fi
            ;;
        *)
            set -- "$@" "$1"
            shift
            left=$((left - 1))
            ;;
    esac
done

# Pass 2: with -r, splice each directory operand into its sorted file
# list (find prints "$dir/..." so prefixes stay relative, as the
# stage docs require). Flags and -E/--color values pass through.
if [ "$HAD_R" = 1 ]; then
    OP_BEFORE=0
    OP_AFTER=0
    left=$#
    SKIP_NEXT=0
    while [ "$left" -gt 0 ]; do
        if [ "$SKIP_NEXT" = 1 ]; then
            set -- "$@" "$1"
            shift
            left=$((left - 1))
            SKIP_NEXT=0
            continue
        fi
        case "$1" in
            -E|--color)
                set -- "$@" "$1"
                shift
                left=$((left - 1))
                SKIP_NEXT=1
                ;;
            --color=*|-r|-o)
                set -- "$@" "$1"
                shift
                left=$((left - 1))
                ;;
            -*)
                set -- "$@" "$1"
                shift
                left=$((left - 1))
                ;;
            *)
                OP_BEFORE=$((OP_BEFORE + 1))
                if [ -d "$1" ]; then
                    D="$1"
                    TMP=$(mktemp)
                    find "$D" -type f | LC_ALL=C sort >"$TMP"
                    while IFS= read -r f; do
                        set -- "$@" "$f"
                        OP_AFTER=$((OP_AFTER + 1))
                    done <"$TMP"
                    rm -f "$TMP"
                    shift
                    left=$((left - 1))
                else
                    set -- "$@" "$1"
                    OP_AFTER=$((OP_AFTER + 1))
                    shift
                    left=$((left - 1))
                fi
                ;;
        esac
    done
    # A directory tree with no files: keep grep.lox out of stdin mode
    # so it exits 1 instead of blocking on input.
    if [ "$OP_BEFORE" -gt 0 ] && [ "$OP_AFTER" -eq 0 ]; then
        set -- "$@" "/dev/null"
    fi
fi

# Pass 3: --color=always needs real ESC bytes, via the environment.
for a in "$@"; do
    if [ "$a" = "--color=always" ]; then
        LOXPP_COLOR_OPEN=$(printf '\033[01;31m')
        LOXPP_COLOR_CLOSE=$(printf '\033[m')
        export LOXPP_COLOR_OPEN LOXPP_COLOR_CLOSE
        break
    fi
done

exec "$LOXPP" "$SCRIPT" "$@"
