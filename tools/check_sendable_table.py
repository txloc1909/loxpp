#!/usr/bin/env python3
"""Checks the sendable table in spec/03-types.md, section "Messages".

The table must have one row for every value kind, and each row must agree
with the message rule written above it. The set of value kinds is the
`type()` table in spec/05-stdlib.md, so a kind added there with no row here
fails, and a row removed here fails.

The message rule is read from its three bullet lines:

    - A value of kind A, B, or C is always a message.
    - A value of kind E is a message when each of its fields is a message.
    - A value of any other kind is never a message.

Usage:
    tools/check_sendable_table.py [--types PATH] [--stdlib PATH]
"""

import argparse
import re

from spec_tables import SpecError, code, report, section, spec_path, table

ALWAYS = "always"
WHEN_FIELDS = "when each field is a message"
NEVER = "never"


def kind_list(text: str) -> list[str]:
    """Splits "A, B, or C" into ["A", "B", "C"]."""
    parts = re.split(r",\s*(?:or\s+)?|\s+or\s+", text.strip())
    return [p.strip() for p in parts if p.strip()]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--types", help="path to a copy of spec/03-types.md")
    parser.add_argument("--stdlib", help="path to a copy of spec/05-stdlib.md")
    args = parser.parse_args()

    problems: list[str] = []
    try:
        types_text = spec_path("03-types.md", args.types).read_text(encoding="utf-8")
        stdlib_text = spec_path("05-stdlib.md", args.stdlib).read_text(encoding="utf-8")

        type_rows = table(section(stdlib_text, "## `type(value) → String`"), "`value` is a...")
        kinds = [re.fullmatch(r'`"(\w+)"`', r["Returns"]).group(1) for r in type_rows]

        messages = section(types_text, "## Messages")
        always_m = re.search(r"^- A value of kind (.+) is always a message\.$", messages, re.M)
        fields_m = re.search(
            r"^- A value of kind (.+) is a message when each of its fields is a message\.$",
            messages,
            re.M,
        )
        never_m = re.search(r"^- A value of any other kind is never a message\.$", messages, re.M)
        if not (always_m and fields_m and never_m):
            raise SpecError("the three bullet lines of the message rule are not all present")
        always = kind_list(always_m.group(1))
        when_fields = kind_list(fields_m.group(1))

        rows = table(messages, "Kind")
    except (SpecError, AttributeError) as exc:
        report("check_sendable_table.py", [f"cannot read the spec: {exc}"], "")
        return

    seen: dict[str, str] = {}
    for row in rows:
        try:
            kind = code(row["Kind"])
        except SpecError as exc:
            problems.append(str(exc))
            continue
        if kind in seen:
            problems.append(f"kind {kind!r} has more than one row")
        seen[kind] = row["Message"]

    for kind in kinds:
        if kind not in seen:
            problems.append(f"kind {kind!r} is in the type() table but has no row in the sendable table")
    for kind in seen:
        if kind not in kinds:
            problems.append(f"kind {kind!r} has a row but is not a kind in the type() table")

    for kind in always + when_fields:
        if kind not in kinds:
            problems.append(f"the message rule names {kind!r}, which is not a kind in the type() table")
    for kind in set(always) & set(when_fields):
        problems.append(f"the message rule puts {kind!r} in two bullet lines")

    for kind, bit in seen.items():
        expected = ALWAYS if kind in always else WHEN_FIELDS if kind in when_fields else NEVER
        if bit != expected:
            problems.append(
                f"kind {kind!r}: the table says {bit!r}, the message rule says {expected!r}"
            )

    report(
        "check_sendable_table.py",
        problems,
        f"{len(seen)} kinds, {len(always)} always, {len(when_fields)} by fields, "
        f"{len(seen) - len(always) - len(when_fields)} never; the table matches the rule",
    )


if __name__ == "__main__":
    main()
