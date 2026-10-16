#!/usr/bin/env python3
"""Checks the Blocking calls table in spec/05-stdlib.md.

Each stdlib section whose call blocks its worker carries the line
`**Blocks:** yes`. The table must list exactly the calls of those sections,
one row per call. A row removed from the table fails, and so does a marker
with no row.

A method heading such as `s.read() -> String` under "## Socket methods" names
the call `Socket.read`; a heading that names several calls marks every one
of them.

Usage:
    tools/check_blocking_calls.py [--stdlib PATH]
"""

import argparse
import re

from spec_tables import SpecError, code, report, section, spec_path, table

CALL = re.compile(r"^(?:(\w+)\.)?(\w+)\(")


def call_name(text: str, receiver: str | None) -> str | None:
    """Returns the call name a code span names, or None if it names none.

    A one-letter prefix is the receiver variable of a methods section.
    """
    m = CALL.match(text)
    if not m:
        return None
    prefix, name = m.group(1), m.group(2)
    if prefix is None:
        return name
    if len(prefix) == 1 and receiver:
        return f"{receiver}.{name}"
    return f"{prefix}.{name}"


def marked_calls(text: str) -> list[str]:
    """Returns every call whose section carries the Blocks marker."""
    marked: list[str] = []
    receiver: str | None = None
    current: list[str] = []
    for line in text.splitlines():
        heading = re.match(r"^(#{2,3}) (.*)$", line)
        if heading:
            level, title = len(heading.group(1)), heading.group(2)
            if level == 2:
                m = re.match(r"^(\w+) methods$", title)
                receiver = m.group(1) if m else None
            current = []
            for span in re.findall(r"`([^`]+)`", title):
                name = call_name(span, receiver)
                if name:
                    current.append(name)
            continue
        if re.match(r"^\*\*Blocks:\*\* yes\s*$", line):
            if not current:
                raise SpecError(f"a Blocks marker sits under a heading that names no call: {line!r}")
            marked.extend(current)
    return marked


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--stdlib", help="path to a copy of spec/05-stdlib.md")
    args = parser.parse_args()

    problems: list[str] = []
    try:
        text = spec_path("05-stdlib.md", args.stdlib).read_text(encoding="utf-8")
        rows = table(section(text, "## Blocking calls"), "Call")
        marked = marked_calls(text)
    except SpecError as exc:
        report("check_blocking_calls.py", [f"cannot read the spec: {exc}"], "")
        return

    listed: list[str] = []
    for row in rows:
        try:
            name = call_name(code(row["Call"]), None)
        except SpecError as exc:
            problems.append(str(exc))
            continue
        if name is None:
            problems.append(f"row {row['Call']!r} does not name a call")
            continue
        if name in listed:
            problems.append(f"call {name!r} has more than one row")
        listed.append(name)

    for name in marked:
        if name not in listed:
            problems.append(f"the section of {name!r} says it blocks, but the table has no row for it")
    for name in listed:
        if name not in marked:
            problems.append(f"the table lists {name!r}, but its section has no '**Blocks:** yes' line")

    report(
        "check_blocking_calls.py",
        problems,
        f"{len(listed)} blocking calls; the table matches the section markers",
    )


if __name__ == "__main__":
    main()
