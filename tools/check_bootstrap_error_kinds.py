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
`.setError(` call in file order and compares each one's kind and message
against EXPECTED_CALLS below -- a reviewed baseline, one entry per call, in
the same order the calls appear in the file. This is a positive manifest,
not a denylist: it fails on a call whose kind reverted to a table string it
used to alias, on a different table kind substituted for the same
non-table cause, on a brand-new call inserted anywhere that was never
reviewed, on a typo in one of this audit's own new non-table strings, and
on a message-only edit at an already-reviewed site that recreates a
table-kind collision without changing that site's kind -- because every
one of those changes the call's actual (kind, message) pair, or the call
count, away from what was reviewed, at that position. It also re-derives
the spec's table kinds from spec/04-semantics.md and fails if any of this
audit's own non-table strings has since become a real table kind, so the
two files cannot silently drift into a new collision this script would
otherwise miss. It cannot, by itself, decide whether some brand-new call's
chosen kind matches its cause -- that judgment is what a human audit
(--report, plus reading spec/04-semantics.md's Runtime Errors table) has to
make before adding it to EXPECTED_CALLS as a JUDGED entry.

--report prints every `.setError(` call site in file order (line number,
whether its kind is a spec/04-semantics.md table kind, the baseline's
JUDGED/RECORDED status, the kind, and the message argument's leading text),
flags any line whose kind or message differs from EXPECTED_CALLS, and
prints the derived totals at the end -- computed from the data, not
hardcoded here. It always exits 0; it is the tool a human audit runs to
re-derive the ground truth and to regenerate EXPECTED_CALLS after a
deliberate, reviewed change.
"""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
INTERPRETER_PATH = REPO_ROOT / "bootstrap" / "loxpp_interpreter.lox"
SPEC_PATH = REPO_ROOT / "spec" / "04-semantics.md"

DYNAMIC = "<dynamic>"
JUDGED = "JUDGED"
RECORDED = "RECORDED"

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
    "ReflectionArityError",
    "LengthTypeError",
    "ValueNotFoundError",
    "StringImmutableError",
    "NotSliceableError",
    "SliceIndexTypeError",
    "SliceIndexNotIntegerError",
    "SliceIndexNegativeError",
    "InLeftOperandTypeError",
    "InRightOperandTypeError",
    "UndefinedMemberError",
    "InvalidFieldReceiverError",
    "ForInNotIterableError",
    "MapSizeChangedError",
    "InvalidSuperclassError",
    "InvalidDestructureReceiverError",
    "ReflectionNotCallableError",
    "ReflectionNativeArityError",
}

# The reviewed baseline: one (kind, message, status) entry per `.setError(`
# call in bootstrap/loxpp_interpreter.lox, in file order.
#
# `kind` is DYNAMIC for a call whose `kind` argument is not a plain string
# literal (its actual kind is chosen at run time). The choices today are
# both between real table kinds: InvalidMapKeyError vs. NaNKeyError for a
# Map key check, and ArityError vs. ConstructorArityError for a call-arity
# check. DYNAMIC only means this script cannot read the choice statically;
# it does not mean the choice is unaudited.
#
# `message` is the call's message argument, exactly as find_set_error_calls
# returns it (message_raw): the literal source text between the commas,
# quotes included, unevaluated. Checking it, not only `kind`, closes the gap
# a kind-only baseline leaves open -- a message-only edit at an existing
# call can recreate the exact collision this audit fixes (the cause
# changes, the `kind` string does not) and a kind-only comparison would not
# notice.
#
# `status` is JUDGED when a person has compared this call's cause against
# its `kind`'s row in spec/04-semantics.md's table (or confirmed the `kind`
# is deliberately non-table, for the reason given in the code comment at
# that site), and RECORDED when the baseline only certifies that this is
# what the call passes today -- it makes no claim that the pairing is
# correct.
#
# Regenerate this list with --report after any reviewed, deliberate change
# to a call's kind, message, or the call count. Flip a RECORDED entry to
# JUDGED only after actually judging that specific site against the spec
# table, never as part of an unrelated change.
EXPECTED_CALLS = [
    ('StackOverflowError', '"Stack overflow."', JUDGED),
    ('ReflectionArityError', '"Expected at least 2 arguments."', JUDGED),
    ('ReflectionReceiverError', '"Only instances have methods."', JUDGED),
    ('ReflectionFieldNameError', '"Field name must be a string."', JUDGED),
    ('ReflectionUndefinedMemberError', '"Undefined property \'" + name + "\'."', JUDGED),
    ('ReflectionUnsupportedError', '"callMethod does not support user-defined methods yet."', JUDGED),
    ('ReflectionNotCallableError', '"callMethod requires a callable value."', JUDGED),
    ('ReflectionNativeArityError', '"Expected " + str(callee.arity()) + " arguments but got " +\n                         str(len(forwarded)) + "."', JUDGED),
    ('LengthTypeError', '"len() argument must be a String, List, or Map."', JUDGED),
    ('ReflectionReceiverError', '"Expected an instance."', JUDGED),
    ('ReflectionReceiverError', '"Expected a class."', JUDGED),
    ('ReflectionReceiverError', '"Only instances have properties."', JUDGED),
    ('ReflectionFieldNameError', '"Field name must be a string."', JUDGED),
    ('ReflectionReceiverError', '"Only instances have properties."', JUDGED),
    ('ReflectionFieldNameError', '"Field name must be a string."', JUDGED),
    ('ReflectionReceiverError', '"Only instances have fields."', JUDGED),
    ('ReflectionFieldNameError', '"Field name must be a string."', JUDGED),
    ('EmptyListError', '"Cannot pop from an empty list."', RECORDED),
    ('ValueNotFoundError', '"Value not found in list."', JUDGED),
    ('NaNKeyError', '"NaN cannot be used as a map key."', RECORDED),
    ('InvalidMapKeyError', '"Map keys must be Bool, Number, Nil, or String."', RECORDED),
    ('IndexTypeError', '"List index must be a number."', RECORDED),
    ('IndexNotIntegerError', '"List index must be an integer."', RECORDED),
    ('IndexOutOfBoundsError', '"List index out of bounds."', RECORDED),
    ('IndexTypeError', '"String index must be a number."', RECORDED),
    ('IndexNotIntegerError', '"String index must be an integer."', RECORDED),
    ('IndexOutOfBoundsError', '"String index out of bounds."', RECORDED),
    ('NotIndexableError', '"Only lists, strings, and maps can be indexed."', RECORDED),
    ('IndexTypeError', '"List index must be a number."', RECORDED),
    ('IndexNotIntegerError', '"List index must be an integer."', RECORDED),
    ('IndexOutOfBoundsError', '"List index out of bounds."', RECORDED),
    ('StringImmutableError', '"Strings are immutable."', JUDGED),
    ('NotIndexableError', '"Only lists and maps support index assignment."', RECORDED),
    ('NotSliceableError', '"Slice requires a List or String."', JUDGED),
    ('SliceIndexTypeError', '"Slice index must be a number."', JUDGED),
    ('SliceIndexNotIntegerError', '"Slice index must be an integer."', JUDGED),
    ('SliceIndexNegativeError', '"Slice index must be non-negative."', JUDGED),
    ('SliceIndexTypeError', '"Slice index must be a number."', JUDGED),
    ('SliceIndexNotIntegerError', '"Slice index must be an integer."', JUDGED),
    ('SliceIndexNegativeError', '"Slice index must be non-negative."', JUDGED),
    ('ForInNotIterableError', '"Value is not iterable (expected list, string, or map)."', JUDGED),
    ('MapSizeChangedError', '"Map changed size during iteration."', JUDGED),
    ('MapSizeChangedError', '"Map changed size during iteration."', JUDGED),
    ('InvalidSuperclassError', '"Superclass must be a class."', JUDGED),
    ('UndefinedVariableError', '"Undefined variable."', RECORDED),
    ('UndefinedVariableError', '"Undefined variable."', RECORDED),
    ('ArithmeticTypeError', '"Operand must be a number."', RECORDED),
    ('ConcatenationTypeError', '"Operands must be two numbers, two strings, or a string and a number."', RECORDED),
    ('ArithmeticTypeError', '"Operands must be numbers."', RECORDED),
    ('ArithmeticTypeError', '"Operands must be numbers."', RECORDED),
    ('ArithmeticTypeError', '"Operands must be numbers."', RECORDED),
    ('ComparisonTypeError', '"Operands must be numbers."', RECORDED),
    ('ComparisonTypeError', '"Operands must be numbers."', RECORDED),
    ('ComparisonTypeError', '"Operands must be numbers."', RECORDED),
    ('ComparisonTypeError', '"Operands must be numbers."', RECORDED),
    ('ArithmeticTypeError', '"Operands must be numbers."', RECORDED),
    ('InLeftOperandTypeError', '"Left operand of \'in\' on a String must be a String."', JUDGED),
    ('InRightOperandTypeError', '"Right operand of \'in\' must be a List, String, or Map."', JUDGED),
    ('NotCallableError', '"Can only call functions, classes and enums."', RECORDED),
    ('NotCallableError', '"Can only call functions, classes and enums."', RECORDED),
    ('ConstructorArityError', '"Constructor called with wrong arity."', RECORDED),
    ('ArityError', '"Expected " + str(callee.arity()) + " arguments but got " + str(len(args)) + "."', RECORDED),
    ('UndefinedMemberError', '"Undefined property \'" + name + "\'."', JUDGED),
    ('UndefinedMemberError', '"Undefined property \'" + name + "\'."', JUDGED),
    ('UndefinedMemberError', '"Undefined property \'" + name + "\'."', JUDGED),
    ('UndefinedPropertyError', '"Undefined property on error."', RECORDED),
    ('InvalidReceiverError', '"Method called on invalid receiver."', JUDGED),
    ('UndefinedMemberError', '"Undefined property \'" + name + "\'."', JUDGED),
    ('InvalidFieldReceiverError', '"Only instances have fields."', JUDGED),
    ('UndefinedMemberError', '"Undefined property \'" + method + "\'."', JUDGED),
    ('MaxDepthExceededError', '"Value nesting is too deep."', RECORDED),
    ('NotIndexableError', '"Sequence destructuring requires a List."', RECORDED),
    ('IndexOutOfBoundsError', '"Not enough elements for sequence destructuring."', RECORDED),
    ('InvalidDestructureReceiverError', '"Object destructuring requires an instance."', JUDGED),
    ('MatchError', '"No matching arm in match expression."', RECORDED),
]

# The exact number of rows load_spec_table_kinds() expects to find in
# spec/04-semantics.md's Runtime Errors table today. A change here means the
# spec table itself changed since the last audit. A table that has grown,
# shrunk, or renamed a row calls every JUDGED site's review back into
# question, so this script fails rather than silently keep trusting a stale
# review. See --report for the current judged/recorded split; this script
# does not restate that split in prose here, to avoid a second hand-written
# copy of a number the data already holds.
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
        judged = sum(1 for _, _, status in EXPECTED_CALLS if status == JUDGED)
        recorded = len(EXPECTED_CALLS) - judged
        judged_table = sum(
            1
            for kind, _, status in EXPECTED_CALLS
            if status == JUDGED and kind in spec_kinds
        )
        table = sum(1 for c in calls if effective_kind(c) in spec_kinds)
        non_table_literal = sum(
            1
            for c in calls
            if c["kind_literal"] is not None and c["kind_literal"] not in spec_kinds
        )
        dynamic = sum(1 for c in calls if c["kind_literal"] is None)

        print(f"{len(calls)} total .setError( calls, {len(spec_kinds)} spec table kinds\n")
        for i, c in enumerate(calls):
            kind = effective_kind(c)
            if c["kind_literal"] is None:
                # A DYNAMIC call's real kind is chosen at run time between
                # real table kinds (see EXPECTED_CALLS's own comment on
                # DYNAMIC), so it is neither table nor non-table by
                # construction; tag it distinctly rather than fall into
                # "non-table" through effective_kind()'s placeholder string.
                tag = "dynamic"
            else:
                tag = "table" if kind in spec_kinds else "non-table"
            if i < len(EXPECTED_CALLS):
                exp_kind, exp_message, status = EXPECTED_CALLS[i]
            else:
                exp_kind, exp_message, status = "<no baseline entry>", "<no baseline entry>", "?"
            flag = ""
            if kind != exp_kind or c["message_raw"] != exp_message:
                flag = f"  ** baseline says {exp_kind!r} / {exp_message[:40]!r} **"
            msg_preview = c["message_raw"][:60].replace("\n", " ")
            print(
                f"  line {c['line']:>5}  [{tag:>9}]  [{status:>8}]  {kind:<32}  {msg_preview}{flag}"
            )
        if len(calls) != len(EXPECTED_CALLS):
            print(
                f"\n** call count is {len(calls)}, EXPECTED_CALLS has {len(EXPECTED_CALLS)} "
                "entries -- a call was added or removed since the last reviewed baseline **"
            )
        distinct = sorted({c["kind_literal"] for c in calls if c["kind_literal"] is not None})
        print(f"\n{len(distinct)} distinct kind strings in use: {', '.join(distinct)}")
        print(
            f"\ntotals: {len(calls)} total, {table} table, {non_table_literal} non-table "
            f"literal, {dynamic} dynamic; {judged} judged ({judged_table} of them table-kind), "
            f"{recorded} recorded"
        )
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
            "audit's EXPECTED_CALLS baseline was reviewed against. Re-run --report, "
            "re-audit every table-tagged call against the new table, and update "
            "EXPECTED_SPEC_TABLE_KIND_COUNT deliberately."
        )

    if len(calls) != len(EXPECTED_CALLS):
        failures.append(
            f"bootstrap/loxpp_interpreter.lox now has {len(calls)} .setError( call(s), "
            f"but the reviewed baseline (EXPECTED_CALLS) has {len(EXPECTED_CALLS)}. A call "
            "was added or removed. Run --report, review every new or removed call's kind "
            "and message against spec/04-semantics.md's table, and update EXPECTED_CALLS "
            "deliberately."
        )
    else:
        for i, c in enumerate(calls):
            actual_kind = effective_kind(c)
            actual_message = c["message_raw"]
            expected_kind, expected_message, _status = EXPECTED_CALLS[i]
            if actual_kind != expected_kind or actual_message != expected_message:
                failures.append(
                    f"bootstrap/loxpp_interpreter.lox:{c['line']}: kind is {actual_kind!r} "
                    f"(message {actual_message!r}), but the reviewed baseline expects kind "
                    f"{expected_kind!r} (message {expected_message!r}) at this position. If "
                    "this change is deliberate and reviewed against spec/04-semantics.md's "
                    "table, update EXPECTED_CALLS; otherwise this is the collision this "
                    "audit exists to catch."
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
