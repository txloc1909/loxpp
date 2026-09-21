#!/bin/bash
# diff_grep.sh — differential oracle: Lox++ grep vs system GNU grep -E.
# Compares stdout (exact bytes) and exit code over a pattern x input battery.
# Usage: tests/diff_grep.sh [your_grep.sh]
PROG="${1:-$(dirname "$0")/../your_grep.sh}"
case "$PROG" in
    /*) ;;
    *) PROG="$(cd "$(dirname "$0")/.." && pwd)/your_grep.sh" ;;
esac
PASS=0
FAIL=0
TMPD=$(mktemp -d)
trap 'rm -rf "$TMPD"' EXIT
# GNU ERE has no \d / \w (it reads them as literal d/w); translate the
# Codecrafters dialect to POSIX classes for the GNU side only.
gnu_pat() {
    printf '%s' "$1" | sed -e 's/\\d/[0-9]/g' -e 's/\\w/[_[:alnum:]]/g'
}

PATTERNS=(
    'a' '\d' '\w' '[abc]' '[^abc]' '[a-z]' '[^0-9]'
    '\d apple' '\d\d\d apple' '\d \w\w\ws'
    '^log' 'dog$' '^...$' '^$' '$' '^'
    'a+' 'a*' 'a?' 'ab+c' 'ab*c' 'ab?c'
    'd.g' '...' 'a.c'
    '(cat|dog)' '(a|b|c)d' '(ab)+'
    '(cat) and \1' '(\w+) \1' '(a)(b) \2\1'
    'a{2}' 'a{3}' 'a{2,}' 'a{1,3}' 'a{2,4}' '\d{3}' '\w+@\w+'
    'colou?r' 'dogs?' 'appl.*' 'b.*$' '.*er' '.*ar'
    '0x[0-9a-fA-F]+' '\d+\.\d+'
    '(ab|cd)e+f?' '^(a|b)+$'
)
INPUTS=(
    'apple' 'dog' '1' 'a' 'foo101' '$!?'
    '1 apple' '1 orange' '100 apples' '3 dogs' '4 cats' '1 dog'
    'log' 'slog' 'dogs' 'SaaS' 'cog' '' '   '
    'cat and cat' 'cat and dog' 'hello hello' 'ab ba'
    'aa' 'aaa' 'aaaa' 'a' 'b' 'abc' 'adc' 'ac'
    'color' 'colour' 'user@example' '12.5x' '0x1f3Z'
    'The king had 7 daughters' 'no match here'
    'strawberry' 'celery' 'cucumber' 'pear' 'carrot' 'corn'
)
printf '%s\n' "${INPUTS[@]}" >"$TMPD/lines.txt"

for pat in "${PATTERNS[@]}"; do
    g_out=$(grep -E -e "$(gnu_pat "$pat")" "$TMPD/lines.txt" 2>/dev/null)
    g_rc=$?
    l_out=$("$PROG" -E "$pat" "$TMPD/lines.txt" 2>/dev/null)
    l_rc=$?
    if [ "$g_out" = "$l_out" ] && [ "$g_rc" = "$l_rc" ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        printf 'DIVERGE pattern=%q\n  gnu rc=%s out=%q\n  lox rc=%s out=%q\n' \
            "$pat" "$g_rc" "$g_out" "$l_rc" "$l_out"
    fi
    # stdin (single line, no trailing newline) — exit code only parity,
    # since GNU preserves the missing newline and print adds one; the
    # stripped comparison below still holds via command substitution.
    for line in 'apple' '1 apple' 'cat and cat' 'zzz' ''; do
        g1=$(printf '%s' "$line" | grep -E -e "$(gnu_pat "$pat")" 2>/dev/null)
        g1r=$?
        l1=$(printf '%s' "$line" | "$PROG" -E "$pat" 2>/dev/null)
        l1r=$?
        if [ "$g1" = "$l1" ] && [ "$g1r" = "$l1r" ]; then
            PASS=$((PASS + 1))
        else
            FAIL=$((FAIL + 1))
            printf 'DIVERGE stdin pattern=%q line=%q\n  gnu rc=%s out=%q\n  lox rc=%s out=%q\n' \
                "$pat" "$line" "$g1r" "$g1" "$l1r" "$l1"
        fi
    done
done

# -o mode parity (whole stdout)
for pat in '\d+' 'cat' '(cat|dog)' '\w+'; do
    g_out=$(grep -E -o -e "$(gnu_pat "$pat")" "$TMPD/lines.txt" 2>/dev/null)
    g_rc=$?
    l_out=$(printf '%s\n' "${INPUTS[@]}" | "$PROG" -o -E "$pat" 2>/dev/null)
    l_rc=$?
    if [ "$g_out" = "$l_out" ] && [ "$g_rc" = "$l_rc" ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        printf 'DIVERGE -o pattern=%q\n  gnu rc=%s out=%q\n  lox rc=%s out=%q\n' \
            "$pat" "$g_rc" "$g_out" "$l_rc" "$l_out"
    fi
done

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
