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

With no arguments it is the permanent regression check: it fails if any
`.setError(` call reuses one of a fixed list of documented
spec/04-semantics.md table `kind` strings for one of the specific,
previously-mismatched causes recorded in KNOWN_COLLISIONS below (each
entry is a scoped region -- the reflection-API branches of
`reflectCallMethod`/`LoxNative.call`, and `InterpListMethod.call`'s
"remove" branch -- paired with the exact old `kind` and a message
substring). It does not, and cannot, decide on its own whether some
future, unrelated `.setError(` call's `kind` matches that kind's
documented cause; that judgment is exactly what a human audit (--report,
plus reading spec/04-semantics.md's Runtime Errors table) has to make.
Keeping the fixed list here turns that one-time audit into a standing
regression guard: if any of these specific causes is ever changed back to
reuse the table kind it used to alias, this check fails.

--report prints every `.setError(` call site in file order (line number,
`kind`, and the message argument's leading text), grouped by whether the
`kind` is one of the 19 spec/04-semantics.md table kinds or not. It always
exits 0; it is the tool a human audit runs to re-derive the ground truth,
not the check ctest runs.
"""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
INTERPRETER_PATH = REPO_ROOT / "bootstrap" / "loxpp_interpreter.lox"
SPEC_PATH = REPO_ROOT / "spec" / "04-semantics.md"

# Each header locates one function or branch body by its opening line, as a
# regex ending on the block's own opening '{' (so an unrelated same-text
# line missing the brace -- e.g. LoxNative.arity()'s
# `if (this.name == "setField") return 3;` -- cannot match it). The regex
# must match exactly one place in the file; compute_regions() raises if it
# does not.
REGION_HEADERS = [
    ("reflectCallMethod", re.compile(r"fun\s+reflectCallMethod\s*\([^)]*\)\s*\{")),
    ("LoxNative.call/getField", re.compile(r'if\s*\(this\.name\s*==\s*"getField"\)\s*\{')),
    ("LoxNative.call/hasField", re.compile(r'if\s*\(this\.name\s*==\s*"hasField"\)\s*\{')),
    ("LoxNative.call/setField", re.compile(r'if\s*\(this\.name\s*==\s*"setField"\)\s*\{')),
    ("InterpListMethod.call/remove", re.compile(r'if\s*\(this\.name\s*==\s*"remove"\)\s*\{')),
]

# Each entry: (region_label, forbidden_kind, message_substring). A call
# fails only when it is both inside the named region (see REGION_HEADERS)
# and matches the (kind, message substring) pair -- plain substring
# matching alone is not enough, because some of these message texts (e.g.
# "Undefined property '" + name + "'.") are shared, word for word, with
# unrelated, already-correct UndefinedPropertyError sites elsewhere in the
# file that must not be flagged.
KNOWN_COLLISIONS = [
    # A reflection field-name argument that is not a String. Nothing to do
    # with a Map literal's key, which is what InvalidMapKeyError documents.
    ("reflectCallMethod", "InvalidMapKeyError", "Field name must be a string"),
    ("LoxNative.call/getField", "InvalidMapKeyError", "Field name must be a string"),
    ("LoxNative.call/hasField", "InvalidMapKeyError", "Field name must be a string"),
    ("LoxNative.call/setField", "InvalidMapKeyError", "Field name must be a string"),
    # reflectCallMethod: no field or method by that name on the instance.
    # spec/04-semantics.md's UndefinedPropertyError documents property
    # access on a caught Error value specifically, not reflection lookup
    # on an arbitrary instance.
    ("reflectCallMethod", "UndefinedPropertyError", "Undefined property"),
    # reflectCallMethod: the found member is a real, callable function/
    # class/enum constructor that this reflection API's v1 scope refuses
    # to invoke -- an unsupported-operation cause, not "the value is not
    # callable" (NotCallableError's documented cause).
    ("reflectCallMethod", "NotCallableError", "does not support user-defined methods"),
    # InterpListMethod.call's "remove" branch: no element equals the
    # argument. Not an index fault -- IndexOutOfBoundsError documents an
    # out-of-range numeric index, and remove() takes a value, not an index.
    ("InterpListMethod.call/remove", "IndexOutOfBoundsError", "Value not found in list"),
]


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


def find_matching_brace(stripped: str, open_idx: int) -> int:
    """Returns the offset of the '}' that closes the '{' at `open_idx` in
    `stripped`, counting nested braces (string contents already blanked by
    strip_string_literals, so a brace-like character inside a message
    argument can never be mistaken for a real one)."""
    depth = 0
    i = open_idx
    while i < len(stripped):
        if stripped[i] == "{":
            depth += 1
        elif stripped[i] == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ValueError(f"unbalanced braces for a block opening at offset {open_idx}")


def compute_regions(text: str, stripped: str) -> dict[str, tuple[int, int]]:
    """Returns {label: (body_start, body_end)} for every REGION_HEADERS
    entry: the character range strictly inside that function's or branch's
    braces. Headers are matched against `text` (they name identifiers like
    "getField" that strip_string_literals blanks out of `stripped`); brace
    matching then runs on `stripped`, at the same offsets, so a brace-like
    character inside some other call's message argument cannot be
    mistaken for a real one. Raises if a header does not match exactly
    once -- silence here would mean a later region check silently checks
    nothing."""
    regions = {}
    for label, pattern in REGION_HEADERS:
        matches = list(pattern.finditer(text))
        if len(matches) != 1:
            raise ValueError(
                f"region header for {label!r} matched {len(matches)} times, expected exactly 1"
            )
        open_idx = matches[0].end() - 1  # the header regex ends on the block's '{'
        close_idx = find_matching_brace(stripped, open_idx)
        regions[label] = (open_idx + 1, close_idx)
    return regions


def find_set_error_calls(text: str) -> list[dict]:
    """Returns every `.setError(...)` call in `text`, each as
    {line, offset, kind_literal, message_raw, arg_count}. `kind_literal` is
    the call's first argument with its surrounding quotes stripped, when
    that argument is a plain string literal (every call site today is); a
    call whose first argument is not a plain string literal gets
    kind_literal None rather than a guess. line is 1-based, the line the
    call's `setError` token starts on; offset is its character offset, for
    matching against compute_regions()'s ranges."""
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
    row -- rather than hardcoding the 19 rows here, so this script and the
    spec table cannot silently drift apart."""
    kinds = set()
    for m in re.finditer(r'\|\s*`"([A-Za-z]+)"`\s*\|', spec_text):
        kinds.add(m.group(1))
    return kinds


def main() -> int:
    report_mode = "--report" in sys.argv[1:]

    text = INTERPRETER_PATH.read_text(encoding="utf-8")
    calls = find_set_error_calls(text)

    if report_mode:
        spec_kinds = load_spec_table_kinds(SPEC_PATH.read_text(encoding="utf-8"))
        print(f"{len(calls)} total .setError( calls, {len(spec_kinds)} spec table kinds\n")
        for c in calls:
            kind = c["kind_literal"] if c["kind_literal"] is not None else "<dynamic>"
            tag = "table" if kind in spec_kinds else "non-table"
            msg_preview = c["message_raw"][:60].replace("\n", " ")
            print(f"  line {c['line']:>5}  [{tag:>9}]  {kind:<32}  {msg_preview}")
        distinct = sorted({c["kind_literal"] for c in calls if c["kind_literal"] is not None})
        print(f"\n{len(distinct)} distinct kind strings in use: {', '.join(distinct)}")
        return 0

    stripped = strip_string_literals(text)
    regions = compute_regions(text, stripped)

    failures = []
    for c in calls:
        if c["kind_literal"] is None:
            continue
        for region_label, forbidden_kind, message_substring in KNOWN_COLLISIONS:
            body_start, body_end = regions[region_label]
            if not (body_start <= c["offset"] < body_end):
                continue
            if c["kind_literal"] == forbidden_kind and message_substring in c["message_raw"]:
                failures.append(
                    f"bootstrap/loxpp_interpreter.lox:{c['line']}: "
                    f'.setError("{forbidden_kind}", ...) inside {region_label} reuses a '
                    f"spec/04-semantics.md table kind for a cause that is not that kind's "
                    f"documented meaning. Give this cause its own non-table kind string."
                )

    if failures:
        print(f"{len(failures)} bootstrap error-kind collision(s) found:\n", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1

    print(f"OK: {len(calls)} .setError( calls checked, 0 known kind collisions.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
