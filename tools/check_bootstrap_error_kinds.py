#!/usr/bin/env python3
"""
Audits bootstrap/loxpp_interpreter.lox's setError() call sites against the
catchable-fault table in spec/04-semantics.md.

setError() is the bootstrap interpreter's only way to deliver a caught
value's `.kind` field. A documented, first-class Lox++ idiom is
`catch (e) { if (e.kind == "...") ... }`, and that idiom breaks silently
the moment two unrelated causes share one `kind` string: both branches of
the `if` see the same text, and nothing at parse time or run time flags
the collision.

This script enumerates every `.setError(` call in the file by balancing
parentheses/brackets and tracking string-literal state character by
character, not by a line-oriented grep or comma count -- a comma inside a
string-literal argument (several call sites build the message with `+`)
silently miscounts a plain grep's tally of arguments or call sites.

Usage:
    tools/check_bootstrap_error_kinds.py            # regression check (ctest)
    tools/check_bootstrap_error_kinds.py --report    # full site-by-site audit

With no arguments it is the permanent regression check. It reads every
`.setError(` call in file order and compares each one's `kind` argument
against EXPECTED_KINDS below -- a reviewed baseline, one entry per call, in
the same order the calls appear in the file. This is a positive manifest,
not a denylist: it fails on a call whose `kind` reverted to a table string
it used to alias, on a *different* table `kind` substituted for the same
non-table cause, on a brand-new call inserted anywhere that was never
reviewed, and on a typo in one of this audit's own new non-table strings --
because every one of those changes the actual kind (or the call count) away
from what was reviewed, at that position. It also re-derives the spec's
table kinds from spec/04-semantics.md and fails if any of this audit's own
non-table strings has since become a real table kind, so the two files
cannot silently drift into a new collision this script would otherwise miss.
It cannot, by itself, decide whether some brand-new call's chosen kind
matches its cause -- that judgment is what a human audit (--report, plus
reading spec/04-semantics.md's Runtime Errors table) has to make before
adding it to EXPECTED_KINDS.

--report prints every `.setError(` call site in file order (line number,
`kind`, and the message argument's leading text), grouped by whether the
`kind` is one of the spec/04-semantics.md table kinds or not, and flags any
line whose kind differs from EXPECTED_KINDS. It always exits 0; it is the
tool a human audit runs to re-derive the ground truth and to regenerate
EXPECTED_KINDS after a deliberate, reviewed change.
"""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
INTERPRETER_PATH = REPO_ROOT / "bootstrap" / "loxpp_interpreter.lox"
SPEC_PATH = REPO_ROOT / "spec" / "04-semantics.md"

DYNAMIC = "<dynamic>"

# Non-table `kind` strings this file's audits have introduced so far, each
# naming one cause the spec/04-semantics.md table does not document (see the
# code comments at each site in bootstrap/loxpp_interpreter.lox for why).
# These must never become a real spec table kind by coincidence; see the
# disjointness check in main().
REVIEWED_NON_TABLE_KINDS = {
    "ReflectionFieldNameError",
    "ReflectionUndefinedMemberError",
    "ReflectionUnsupportedError",
    "ReflectionReceiverError",
    "LengthTypeError",
    "ValueNotFoundError",
}

# The reviewed baseline: one entry per `.setError(` call in
# bootstrap/loxpp_interpreter.lox, in file order, holding the `kind` that
# call is expected to pass -- DYNAMIC for a call whose `kind` argument is not
# a plain string literal (its actual kind is chosen at run time). Two such
# choices exist today, both between real table kinds: InvalidMapKeyError vs.
# NaNKeyError for a Map key's validity check (six sites), and ArityError vs.
# ConstructorArityError for a call-arity check (one site). DYNAMIC only means
# this script cannot read the choice statically; it does not mean the choice
# is unaudited. Regenerate this list with --report after any reviewed,
# deliberate change to a call's `kind` or to the call count, and only then.
EXPECTED_KINDS = [
    "ArityError",
    "InvalidReceiverError",
    "ReflectionFieldNameError",
    "ReflectionUndefinedMemberError",
    "ReflectionUnsupportedError",
    "NotCallableError",
    "ArityError",
    "LengthTypeError",
    "ReflectionReceiverError",
    "ReflectionReceiverError",
    "ReflectionReceiverError",
    "ReflectionFieldNameError",
    "ReflectionReceiverError",
    "ReflectionFieldNameError",
    "ReflectionReceiverError",
    "ReflectionFieldNameError",
    "EmptyListError",
    "ValueNotFoundError",
    DYNAMIC,
    DYNAMIC,
    DYNAMIC,
    DYNAMIC,
    "IndexTypeError",
    "IndexNotIntegerError",
    "IndexOutOfBoundsError",
    "IndexTypeError",
    "IndexNotIntegerError",
    "IndexOutOfBoundsError",
    "NotIndexableError",
    DYNAMIC,
    "IndexTypeError",
    "IndexNotIntegerError",
    "IndexOutOfBoundsError",
    "NotIndexableError",
    "NotIndexableError",
    "NotIndexableError",
    "IndexTypeError",
    "IndexNotIntegerError",
    "IndexOutOfBoundsError",
    "IndexTypeError",
    "IndexNotIntegerError",
    "IndexOutOfBoundsError",
    "NotIndexableError",
    "NotCallableError",
    "UndefinedVariableError",
    "UndefinedVariableError",
    "ArithmeticTypeError",
    "ConcatenationTypeError",
    "ArithmeticTypeError",
    "ArithmeticTypeError",
    "ArithmeticTypeError",
    "ComparisonTypeError",
    "ComparisonTypeError",
    "ComparisonTypeError",
    "ComparisonTypeError",
    "ArithmeticTypeError",
    "IndexTypeError",
    DYNAMIC,
    "NotIndexableError",
    "NotCallableError",
    "NotCallableError",
    "NotCallableError",
    DYNAMIC,
    "UndefinedPropertyError",
    "UndefinedPropertyError",
    "UndefinedPropertyError",
    "UndefinedPropertyError",
    "InvalidReceiverError",
    "UndefinedPropertyError",
    "InvalidReceiverError",
    "UndefinedPropertyError",
    "MaxDepthExceededError",
    "NotIndexableError",
    "IndexOutOfBoundsError",
    "InvalidReceiverError",
    "MatchError",
]

# The exact number of rows load_spec_table_kinds() expects to find in
# spec/04-semantics.md's Runtime Errors table today. A change here means the
# spec table itself changed since the last audit. Only 17 of the 56 calls
# tagged [table] above were judged, site by site, against these 19 rows'
# documented meanings -- the reflection-API and list.remove() call sites
# this file's kind strings were most recently audited for (13 were renamed
# off a table kind onto a non-table one; 4 kept a table kind, correctly).
# The other 52 table-kind calls are recorded here as they stand today; this
# baseline does not certify them. A table that has grown, shrunk, or
# renamed a row calls the 17 judged sites' review back into question, so
# this script fails rather than silently keep trusting a stale review.
EXPECTED_SPEC_TABLE_KIND_COUNT = 19


def strip_string_literals(text: str) -> str:
    """Returns `text` with every double-quoted Lox++ string literal's
    contents replaced by 'x', same length, quotes and backslash escapes
    left in place. Used so paren/bracket/comma scanning never mistakes a
    delimiter-looking character inside a string argument for a real one."""
    out = list(text)
    in_string = False
    escaped = False
    for i, ch in enumerate(text):
        if in_string:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
            else:
                out[i] = "x"
        elif ch == '"':
            in_string = True
    return "".join(out)


def split_top_level_args(stripped_args: str, real_args: str) -> list[str]:
    """Splits a call's argument text (the substring strictly between its
    outer parentheses) at commas that are not nested inside (), [], or a
    string literal. `stripped_args` and `real_args` must be the same
    length; splitting is decided on `stripped_args`, returned slices come
    from `real_args`."""
    parts = []
    depth = 0
    start = 0
    for i, ch in enumerate(stripped_args):
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        elif ch == "," and depth == 0:
            parts.append(real_args[start:i].strip())
            start = i + 1
    parts.append(real_args[start:].strip())
    return parts


def find_set_error_calls(text: str) -> list[dict]:
    """Returns every `.setError(...)` call in `text`, each as
    {line, offset, kind_literal, message_raw, arg_count}. `kind_literal` is
    the call's first argument with its surrounding quotes stripped, when
    that argument is a plain string literal (every call site today is); a
    call whose first argument is not a plain string literal gets
    kind_literal None rather than a guess. line is 1-based, the line the
    call's `setError` token starts on; offset is its character offset."""
    stripped = strip_string_literals(text)
    calls = []
    for m in re.finditer(r"\.setError\(", stripped):
        open_paren = m.end() - 1
        depth = 0
        i = open_paren
        close_paren = None
        while i < len(stripped):
            if stripped[i] == "(":
                depth += 1
            elif stripped[i] == ")":
                depth -= 1
                if depth == 0:
                    close_paren = i
                    break
            i += 1
        if close_paren is None:
            raise ValueError(f"unbalanced parens for a setError( call at offset {open_paren}")
        args = split_top_level_args(
            stripped[open_paren + 1 : close_paren], text[open_paren + 1 : close_paren]
        )
        line = text.count("\n", 0, open_paren) + 1
        kind_literal = None
        if len(args) >= 1:
            lit = re.fullmatch(r'"((?:[^"\\]|\\.)*)"', args[0])
            if lit:
                kind_literal = lit.group(1)
        message_raw = args[1] if len(args) >= 2 else ""
        calls.append(
            {
                "line": line,
                "offset": open_paren,
                "kind_literal": kind_literal,
                "message_raw": message_raw,
                "arg_count": len(args),
            }
        )
    return calls


def load_spec_table_kinds(spec_text: str) -> set[str]:
    """Extracts every `kind` string from spec/04-semantics.md's Runtime
    Errors table -- the last column of each `| Cause | Example | kind |`
    row -- rather than hardcoding the rows here, so this script and the
    spec table cannot silently drift apart."""
    kinds = set()
    for m in re.finditer(r'\|\s*`"([A-Za-z]+)"`\s*\|', spec_text):
        kinds.add(m.group(1))
    return kinds


def effective_kind(call: dict) -> str:
    return call["kind_literal"] if call["kind_literal"] is not None else DYNAMIC


def main() -> int:
    report_mode = "--report" in sys.argv[1:]

    text = INTERPRETER_PATH.read_text(encoding="utf-8")
    calls = find_set_error_calls(text)
    spec_kinds = load_spec_table_kinds(SPEC_PATH.read_text(encoding="utf-8"))

    if report_mode:
        print(f"{len(calls)} total .setError( calls, {len(spec_kinds)} spec table kinds\n")
        for i, c in enumerate(calls):
            kind = effective_kind(c)
            tag = "table" if kind in spec_kinds else "non-table"
            baseline = EXPECTED_KINDS[i] if i < len(EXPECTED_KINDS) else "<no baseline entry>"
            flag = "" if kind == baseline else f"  ** baseline says {baseline!r} **"
            msg_preview = c["message_raw"][:60].replace("\n", " ")
            print(
                f"  line {c['line']:>5}  [{tag:>9}]  {kind:<32}  {msg_preview}{flag}"
            )
        if len(calls) != len(EXPECTED_KINDS):
            print(
                f"\n** call count is {len(calls)}, EXPECTED_KINDS has {len(EXPECTED_KINDS)} "
                "entries -- a call was added or removed since the last reviewed baseline **"
            )
        distinct = sorted({c["kind_literal"] for c in calls if c["kind_literal"] is not None})
        print(f"\n{len(distinct)} distinct kind strings in use: {', '.join(distinct)}")
        return 0

    failures = []

    # A table kind this audit deliberately gave to a non-table cause must
    # never become a real table kind by later, unrelated spec edits -- that
    # would silently recreate the exact ambiguity this file fixes, just with
    # the roles of "old" and "new" reversed.
    drifted = REVIEWED_NON_TABLE_KINDS & spec_kinds
    if drifted:
        failures.append(
            "spec/04-semantics.md now documents "
            + ", ".join(sorted(drifted))
            + " as a table kind, but bootstrap/loxpp_interpreter.lox already uses "
            "that exact string for a non-table cause (see REVIEWED_NON_TABLE_KINDS). "
            "Re-audit every bootstrap call using this kind against the new table row."
        )

    if len(spec_kinds) != EXPECTED_SPEC_TABLE_KIND_COUNT:
        failures.append(
            f"spec/04-semantics.md's Runtime Errors table now yields "
            f"{len(spec_kinds)} kind(s), not the {EXPECTED_SPEC_TABLE_KIND_COUNT} this "
            "audit's EXPECTED_KINDS baseline was reviewed against. Re-run --report, "
            "re-audit every table-tagged call against the new table, and update "
            "EXPECTED_SPEC_TABLE_KIND_COUNT deliberately."
        )

    if len(calls) != len(EXPECTED_KINDS):
        failures.append(
            f"bootstrap/loxpp_interpreter.lox now has {len(calls)} .setError( call(s), "
            f"but the reviewed baseline (EXPECTED_KINDS) has {len(EXPECTED_KINDS)}. A call "
            "was added or removed. Run --report, review every new or removed call's kind "
            "against spec/04-semantics.md's table, and update EXPECTED_KINDS deliberately."
        )
    else:
        for i, c in enumerate(calls):
            actual = effective_kind(c)
            expected = EXPECTED_KINDS[i]
            if actual != expected:
                failures.append(
                    f"bootstrap/loxpp_interpreter.lox:{c['line']}: kind is {actual!r}, "
                    f"but the reviewed baseline expects {expected!r} at this position. "
                    "If this change is deliberate and reviewed against "
                    "spec/04-semantics.md's table, update EXPECTED_KINDS; otherwise this "
                    "is the collision this audit exists to catch."
                )

    if failures:
        print(f"{len(failures)} bootstrap error-kind audit failure(s):\n", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1

    print(
        f"OK: {len(calls)} .setError( calls checked against the reviewed baseline, "
        "0 mismatches."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
