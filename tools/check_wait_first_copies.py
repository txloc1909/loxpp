#!/usr/bin/env python3
"""Checks that every copy of the wait first library is the same text.

Lox++ has no import, so the library of examples/wait_first.lox is also in
each probe that tests it. The library is the text from the line
`// --- wait first library: begin` to the line
`// --- wait first library: end`, both included. This script fails when a file
lacks either marker, has a marker more than once, or has a library text that
differs from the one in the example.

Usage:
    tools/check_wait_first_copies.py

Exit 0 when every copy is the same, 1 otherwise. Pure Python, so it needs no
build.
"""

import difflib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = Path("examples/wait_first.lox")
COPIES = [
    Path("test/translation-probes/297_wait_first_receive.lox"),
    Path("test/translation-probes/298_wait_first_read_limit.lox"),
    Path("test/translation-probes/299_wait_first_limit_after_task_ended.lox"),
]
BEGIN = "// --- wait first library: begin"
END = "// --- wait first library: end"


def library(path: Path) -> list[str] | str:
    """The library lines of path, or an error message."""
    lines = (ROOT / path).read_text(encoding="utf-8").splitlines()
    begins = [i for i, line in enumerate(lines) if line.startswith(BEGIN)]
    ends = [i for i, line in enumerate(lines) if line.startswith(END)]
    if len(begins) != 1 or len(ends) != 1:
        return f"{path}: expected one begin and one end marker, found {len(begins)} and {len(ends)}"
    if ends[0] < begins[0]:
        return f"{path}: the end marker comes before the begin marker"
    return lines[begins[0] : ends[0] + 1]


def main() -> int:
    source = library(SOURCE)
    if isinstance(source, str):
        print(f"error: {source}", file=sys.stderr)
        return 1
    failed = False
    for copy in COPIES:
        text = library(copy)
        if isinstance(text, str):
            print(f"error: {text}", file=sys.stderr)
            failed = True
            continue
        if text != source:
            print(f"error: {copy}: the library differs from {SOURCE}", file=sys.stderr)
            for line in difflib.unified_diff(source, text, str(SOURCE), str(copy), lineterm=""):
                print(f"  {line}", file=sys.stderr)
            failed = True
    if failed:
        return 1
    print(f"OK: {len(COPIES)} copies of the wait first library match {SOURCE}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
