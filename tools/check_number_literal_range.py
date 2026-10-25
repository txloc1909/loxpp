#!/usr/bin/env python3
"""Proves that the table of the range of a Number literal in
spec/01-lexical.md and the spec probes stay in lockstep.

The table lists the boundaries of the rule that a `NUMBER` literal denotes the
nearest double to its decimal: both ends of the range, a decimal just outside
each end that still rounds back to it, the midpoint at each end, a decimal far
outside each end, and zero itself. Each row must have one
`// COVERS: number-literal-range <case>` line in a spec probe, every such line
must name a row, and the `// CHECK:` line after it must equal the row's Value.
A row with no probe, a COVERS line with no row, a duplicate row or COVERS
line, a mismatched value, or a row of EXPECTED_ROWS that is missing fails this
check. EXPECTED_ROWS is the independent ground truth, so a table that loses a
row together with its probe line fails here and not only in review.

Pure Python, no build.
"""

import re
import sys
from pathlib import Path

from check_spec_probes import parse_probe
from spec_tables import (
    PROBE_DIR,
    SpecError,
    code,
    report,
    section,
    spec_path,
    table,
)

SCRIPT = "check_number_literal_range.py"
SPEC_PATH = spec_path("01-lexical.md")
SECTION = "## Number and Int Literals"
COVERS_SET = "number-literal-range"
# Every row of the table, in the order of the spec.
EXPECTED_ROWS = (
    "largest-finite",
    "above-largest-finite",
    "overflow",
    "overflow-midpoint",
    "far-above-range",
    "smallest-subnormal",
    "below-smallest-subnormal",
    "underflow-midpoint",
    "far-below-range",
)
COVERS_RE = re.compile(r"^\s*//\s*COVERS:\s*(\S+)\s+(\S+)\s*$")
CHECK_RE = re.compile(r"^\s*//\s*CHECK:\s?(.*)$")


def spec_rows(problems: list[str]) -> dict[str, str]:
    """Returns {case: expected value} from the spec table."""
    text = SPEC_PATH.read_text(encoding="utf-8")
    rows: dict[str, str] = {}
    for row in table(section(text, SECTION), "Case"):
        case = code(row["Case"])
        if case in rows:
            problems.append(f"the table has more than one row for {case!r}")
            continue
        rows[case] = code(row["Value"])
    return rows


def probe_rows(path: Path, problems: list[str]) -> dict[str, str]:
    """Returns {case: CHECK value} from the COVERS lines of one spec probe."""
    rows: dict[str, str] = {}
    seen: set[str] = set()
    pending = None
    for line in path.read_text(encoding="utf-8").splitlines():
        m = COVERS_RE.match(line)
        if m and m.group(1) == COVERS_SET:
            case = m.group(2)
            if case in seen:
                problems.append(
                    f"{path.name} has more than one COVERS line for {case!r}"
                )
            seen.add(case)
            pending = case
            continue
        m = CHECK_RE.match(line)
        if m and pending is not None:
            rows[pending] = m.group(1).rstrip()
            pending = None
    return rows


def main() -> int:
    problems: list[str] = []
    spec = spec_rows(problems)

    probes: dict[str, tuple[Path, str]] = {}
    for path in sorted(PROBE_DIR.glob("*.lox")):
        lines = path.read_text(encoding="utf-8").splitlines()
        if not any(
            (m := COVERS_RE.match(line)) and m.group(1) == COVERS_SET for line in lines
        ):
            continue
        if parse_probe(path) is None:
            problems.append(f"{path.name}: a COVERS line in a file with no STATUS line")
            continue
        for case, value in probe_rows(path, problems).items():
            if case in probes:
                problems.append(
                    f"case {case!r} has more than one COVERS line "
                    f"({probes[case][0].name} and {path.name})"
                )
                continue
            probes[case] = (path, value)

    for case in EXPECTED_ROWS:
        if case not in spec:
            problems.append(f"the table has no row for {case!r}")

    for case, expected in spec.items():
        if case not in probes:
            problems.append(
                f"spec row {case!r} has no `// COVERS: {COVERS_SET} {case}` "
                f"in a spec probe"
            )
            continue
        path, value = probes[case]
        if value != expected:
            problems.append(
                f"spec row {case!r} expects {expected!r}, but {path.name} "
                f"checks {value!r}"
            )
    for case in probes:
        if case not in spec:
            path = probes[case][0]
            problems.append(
                f"{path.name} has a COVERS line for {case!r}, which is not a "
                f"row of {SPEC_PATH.name}'s table"
            )

    report(
        SCRIPT,
        problems,
        f"{len(EXPECTED_ROWS)} rows, each with a probe whose CHECK matches its row",
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
