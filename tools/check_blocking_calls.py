#!/usr/bin/env python3
"""Checks the Blocking calls table in spec/05-stdlib.md.

Each stdlib section of a blocking call carries one marker line. A call of
class `suspends` (it suspends its task) carries `**Suspends:** yes`; a call of
class `blocks` (it blocks its worker) carries `**Blocks:** yes`. The table must
list exactly the calls of those sections, one row per call, and the `Class`
cell of each row must agree with the marker of its section. A row removed from
the table fails, and so does a marker with no row, a row with no marker, a
marker that disagrees with the class, and a section with both markers.

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


MARKERS = {"Blocks": "blocks", "Suspends": "suspends"}
MARKER_LINE = re.compile(r"^\*\*(Blocks|Suspends):\*\* yes\s*$")


def marked_calls(text: str) -> dict[str, list[str]]:
    """Returns each call whose section carries a marker, mapped to the classes
    its markers name (a well-formed section names exactly one)."""
    marked: dict[str, list[str]] = {}
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
        m = MARKER_LINE.match(line)
        if m:
            if not current:
                raise SpecError(f"a marker sits under a heading that names no call: {line!r}")
            for name in current:
                marked.setdefault(name, []).append(MARKERS[m.group(1)])
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

    listed: dict[str, str] = {}
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
        cls = row["Class"]
        if cls not in MARKERS.values():
            problems.append(f"row {name!r}: Class must be 'suspends' or 'blocks', not {cls!r}")
        listed[name] = cls

    for name, classes in marked.items():
        if name not in listed:
            problems.append(f"the section of {name!r} has a marker, but the table has no row for it")
        if len(classes) > 1:
            problems.append(f"the section of {name!r} has more than one marker")
    for name, cls in listed.items():
        if name not in marked:
            problems.append(f"the table lists {name!r}, but its section has no marker line")
        elif marked[name] != [cls]:
            problems.append(
                f"the table gives {name!r} the class {cls!r}, but its section marker says {marked[name]}"
            )

    report(
        "check_blocking_calls.py",
        problems,
        f"{len(listed)} blocking calls; the class column matches the section markers",
    )


if __name__ == "__main__":
    main()
