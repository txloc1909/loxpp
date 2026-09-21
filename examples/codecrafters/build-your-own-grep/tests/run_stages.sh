#!/bin/bash
# run_stages.sh — public-source test oracle for the Lox++ grep.
#
# Every case is transcribed from codecrafters-io/build-your-own-grep
# stage_descriptions/*.md (public, no membership needed). Asserts exact
# stdout plus exit code through your_grep.sh. A second battery diffs
# against system GNU grep -E as an independent oracle.
#
# Usage: tests/run_stages.sh [your_grep.sh]
PROG="${1:-$(dirname "$0")/../your_grep.sh}"
case "$PROG" in
    /*) ;;
    *) PROG="$(cd "$(dirname "$0")/.." && pwd)/your_grep.sh" ;;
esac
PASS=0
FAIL=0

# check <name> <expected-exit> <expected-stdout> <stdin> -- args...
check() {
    local name="$1" want_rc="$2" want_out="$3" stdin_data="$4"
    shift 4
    local got_out got_rc
    got_out=$(printf '%s' "$stdin_data" | "$PROG" "$@" 2>/dev/null)
    got_rc=$?
    if [ "$got_rc" = "$want_rc" ] && [ "$got_out" = "$want_out" ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        printf 'FAIL %s\n  args: %s\n  want rc=%s out=%q\n  got  rc=%s out=%q\n' \
            "$name" "$*" "$want_rc" "$want_out" "$got_rc" "$got_out"
    fi
}

# ---- base stages (cq2..zm7) ----
check "literal-hit" 0 "apple" "apple" -E "a"
check "literal-miss" 1 "" "dog" -E "a"
check "digit-hit" 0 "1" "1" -E '\d'
check "digit-miss" 1 "" "a" -E '\d'
check "word-hit" 0 "foo101" "foo101" -E '\w'
check "word-miss" 1 "" '$!?' -E '\w'
check "class-pos-hit" 0 "apple" "apple" -E "[abc]"
check "class-pos-miss" 1 "" "dog" -E "[abc]"
check "class-neg-hit" 0 "dog" "dog" -E "[^abc]"
check "class-neg-miss" 1 "" "cab" -E "[^abc]"
check "combo-1-hit" 0 "1 apple" "1 apple" -E '\d apple'
check "combo-1-miss" 1 "" "1 orange" -E '\d apple'
check "combo-2-hit" 0 "100 apples" "100 apples" -E '\d\d\d apple'
check "combo-2-miss" 1 "" "1 apple" -E '\d\d\d apple'
check "combo-3-dogs" 0 "3 dogs" "3 dogs" -E '\d \w\w\ws'
check "combo-3-cats" 0 "4 cats" "4 cats" -E '\d \w\w\ws'
check "combo-3-miss" 1 "" "1 dog" -E '\d \w\w\ws'
check "anchor-start-hit" 0 "log" "log" -E "^log"
check "anchor-start-miss" 1 "" "slog" -E "^log"
check "anchor-end-hit" 0 "dog" "dog" -E 'dog$'
check "anchor-end-miss" 1 "" "dogs" -E 'dog$'
check "plus-apple" 0 "apple" "apple" -E "a+"
check "plus-saas" 0 "SaaS" "SaaS" -E "a+"
check "plus-miss" 1 "" "dog" -E "a+"
check "opt-dogs" 0 "dogs" "dogs" -E "dogs?"
check "opt-dog" 0 "dog" "dog" -E "dogs?"
check "opt-miss" 1 "" "cat" -E "dogs?"
check "dot-hit" 0 "dog" "dog" -E "d.g"
check "dot-miss" 1 "" "cog" -E "d.g"
check "alt-dog" 0 "dog" "dog" -E "(cat|dog)"
check "alt-cat" 0 "cat" "cat" -E "(cat|dog)"
check "alt-miss" 1 "" "apple" -E "(cat|dog)"

# ---- backreferences (sb5,tg1,xe5) ----
check "backref-single-hit" 0 "cat and cat" "cat and cat" -E "(cat) and \1"
check "backref-single-miss" 1 "" "cat and dog" -E "(cat) and \1"
check "backref-multi-hit" 0 "3 red squares and 3 red circles" \
    "3 red squares and 3 red circles" -E '(\d+) (\w+) squares and \1 \2 circles'
check "backref-multi-miss" 1 "" "3 red squares and 4 red circles" \
    -E '(\d+) (\w+) squares and \1 \2 circles'
check "backref-nested" 0 "'cat and cat' is the same as 'cat and cat'" \
    "'cat and cat' is the same as 'cat and cat'" -E "('(cat) and \2') is the same as \1"

# ---- quantifiers (ai9,wy9,hk3,ug0) ----
check "star-basic" 0 "aaab" "aaab" -E "a*"
# One empty line (not zero bytes): a* matches the empty line itself.
check "star-empty-line" 0 "" "
" -E "a*"
check "exact-n-hit" 0 "aa" "aa" -E "a{2}"
check "exact-n-miss" 1 "" "a" -E "a{2}"
check "atleast-hit" 0 "aaa" "aaa" -E "a{2,}"
check "atleast-exact" 0 "aa" "aa" -E "a{2,}"
check "atleast-miss" 1 "" "a" -E "a{2,}"
check "range-hit" 0 "aa" "aa" -E "a{1,2}"
check "range-miss" 1 "" "b" -E "a{1,2}"
check "dollar-star" 0 "apple" "apple" -E "appl.*"

# ---- printing (ku5,pz6): multi-line stdin ----
MULTI_IN=$(printf 'apple\nbanana\ncherry')
check "print-multi" 0 "$(printf 'apple\nbanana')" "$MULTI_IN" -E "a"
check "print-nomatch" 1 "" "$MULTI_IN" -E "zzz"

# ---- file search (dr5,ol9,is6,yx6) ----
TMPD=$(mktemp -d)
trap 'rm -rf "$TMPD"' EXIT
printf 'apple\n' >"$TMPD/fruits.txt"
check "file-single-hit" 0 "apple" "" -E "appl.*" "$TMPD/fruits.txt"
check "file-single-miss" 1 "" "" -E "carrot" "$TMPD/fruits.txt"
printf 'banana\nblueberry\n' >"$TMPD/fruits2.txt"
printf 'broccoli\ncarrot\n' >"$TMPD/veg.txt"
check "file-multi" 0 "$(printf '%s\n%s\n%s' \
    "$TMPD/fruits2.txt:banana" "$TMPD/fruits2.txt:blueberry" "$TMPD/veg.txt:broccoli")" \
    "" -E "b.*$" "$TMPD/fruits2.txt" "$TMPD/veg.txt"
check "file-multi-miss" 1 "" "" -E "missing_fruit" "$TMPD/fruits2.txt" "$TMPD/veg.txt"
mkdir -p "$TMPD/dir/subdir"
printf 'pear\nstrawberry\n' >"$TMPD/dir/fruits.txt"
printf 'celery\ncarrot\n' >"$TMPD/dir/subdir/vegetables.txt"
printf 'cucumber\ncorn\n' >"$TMPD/dir/vegetables.txt"
(
    cd "$TMPD" || exit 1
    got_out=$("$PROG" -r -E ".*er" dir/ 2>/dev/null | LC_ALL=C sort)
    got_rc=${PIPESTATUS[0]}
    want_out=$(printf 'dir/fruits.txt:strawberry\ndir/subdir/vegetables.txt:celery\ndir/vegetables.txt:cucumber')
    if [ "$got_rc" = 0 ] && [ "$got_out" = "$want_out" ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        printf 'FAIL recursive-er\n  want rc=0 out=%q\n  got  rc=%s out=%q\n' "$want_out" "$got_rc" "$got_out"
    fi
    check "recursive-miss" 1 "" "" -r -E "missing_fruit" dir/
)

# ---- -o flag (cj0,ss2,bo4) ----
check "only-single" 0 "7" "The king had 7 daughters" -o -E '\d'
check "only-miss" 1 "" "no digits here" -o -E '\d'
check "only-multi-per-line" 0 "$(printf 'cat\ncat')" "cat cat" -o -E "cat"
OLINES_IN=$(printf 'one cat\ntwo dogs\nthree cats')
check "only-multi-lines" 0 "$(printf 'cat\ndog\ncat')" "$OLINES_IN" -o -E "(cat|dog)"

# ---- --color (bm2,eq0,wg2,jk4) ----
ESC=$(printf '\033')
OPEN="${ESC}[01;31m"
CLOSE="${ESC}[m"
check "color-single" 0 "I have ${OPEN}3${CLOSE} apples" "I have 3 apples" --color=always -E '\d'
check "color-miss" 1 "" "no digits" --color=always -E '\d'
check "color-multi" 0 "${OPEN}3${CLOSE} apples and ${OPEN}4${CLOSE} pears" \
    "3 apples and 4 pears" --color=always -E '\d+'
check "color-never" 0 "I have 3 apples" "I have 3 apples" --color=never -E '\d'
COLORLINES_IN=$(printf 'line 1 here\nline 2 there')
COLOR_WANT=$(printf 'line %s1%s here\nline %s2%s there' "$OPEN" "$CLOSE" "$OPEN" "$CLOSE")
check "color-multiline" 0 "$COLOR_WANT" \
    "$COLORLINES_IN" --color=always -E '\d'
# --color=auto piped (not a TTY) must stay plain
check "color-auto-pipe" 0 "I have 3 apples" "I have 3 apples" --color=auto -E '\d'

# ---- review round 1 regressions ----
# -r prefixes even when the tree holds one file (blocking finding).
mkdir -p "$TMPD/sd"
printf 'apple\n' >"$TMPD/sd/only.txt"
(
    cd "$TMPD" || exit 1
    got_out=$("$PROG" -r -E "apple" sd/ 2>/dev/null)
    got_rc=$?
    if [ "$got_rc" = 0 ] && [ "$got_out" = "sd/only.txt:apple" ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        printf 'FAIL recursive-single-file\n  got rc=%s out=%q\n' "$got_rc" "$got_out"
    fi
)
# --color space form highlights; auto space form stays plain on a pipe.
check "color-space-form" 0 "I have ${OPEN}3${CLOSE} apples" "I have 3 apples" --color always -E '\d'
check "color-auto-space-pipe" 0 "I have 3 apples" "I have 3 apples" --color auto -E '\d'
# -o skips empty spans when a real span exists; else one empty line.
check "only-empty-spans" 0 "aa" "aab" -o -E 'a*'
check "only-all-empty" 0 "" "b" -o -E 'a*'
# Missing files exit 2 and win over a match; dirs without -r exit 2.
check "missing-file" 2 "" "" -E "apple" /nonexistent-xyz-loxpp
printf 'apple\n' >"$TMPD/ok.txt"
check "missing-plus-match" 2 "$TMPD/ok.txt:apple" "" -E "apple" "$TMPD/ok.txt" /nonexistent-xyz-loxpp
check "dir-without-r" 2 "" "" -E "apple" "$TMPD/sd"

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
