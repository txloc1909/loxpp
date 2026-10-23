#!/usr/bin/env python3
"""Proves spec/03-types.md's Number edge table and the number-format probe
stay in lockstep.

The spec table lists every boundary of the Number string form: the two
zeroes, the special values, and the notation thresholds. Each row must have
one `// COVERS: number-format <case>` line in the probe, each COVERS line
must name a spec row, and the probe's `// CHECK:` value for a row must equal
the row's String column. A row with no probe, a probe with no row, or a
mismatched value fails this check.

Pure Python, no build.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPEC_PATH = ROOT / "spec" / "03-types.md"
PROBE_PATH = ROOT / "test" / "translation-probes" / "243_number_format.lox"

COVERS_RE = re.compile(r"^\s*//\s*COVERS:\s*number-format\s+(\S+)\s*$")
CHECK_RE = re.compile(r"^\s*//\s*CHECK:\s?(.*)$")


def spec_rows() -> dict[str, str]:
    """Returns {case: expected string} from the spec edge table."""
    text = SPEC_PATH.read_text(encoding="utf-8")
    start = text.index("The Number form is pinned by this edge table")
    end = text.index("The nesting depth of a value", start)
    rows: dict[str, str] = {}
    for line in text[start:end].splitlines():
        stripped = line.strip()
        if not stripped.startswith("|"):
            continue
        cells = [c.strip() for c in stripped.strip("|").split("|")]
        if not cells or cells[0] == "Case" or set(cells[0]) <= {"-"}:
            continue
        rows[cells[0]] = cells[2].strip("`")
    return rows


def probe_rows() -> dict[str, str]:
    """Returns {case: CHECK value} from the probe, one COVERS then its CHECK."""
    rows: dict[str, str] = {}
    pending = None
    for line in PROBE_PATH.read_text(encoding="utf-8").splitlines():
        m = COVERS_RE.match(line)
        if m:
            pending = m.group(1)
            continue
        m = CHECK_RE.match(line)
        if m and pending is not None:
            rows[pending] = m.group(1).rstrip()
            pending = None
    return rows


def main() -> int:
    spec = spec_rows()
    probe = probe_rows()
    findings: list[str] = []
    for case, expected in spec.items():
        if case not in probe:
            findings.append(
                f"spec row {case!r} has no `// COVERS: number-format {case}` "
                f"in {PROBE_PATH.name}"
            )
        elif probe[case] != expected:
            findings.append(
                f"spec row {case!r} expects {expected!r}, but the probe's "
                f"CHECK is {probe[case]!r}"
            )
    for case in probe:
        if case not in spec:
            findings.append(
                f"{PROBE_PATH.name} covers {case!r}, which is not a row of the "
                "spec edge table"
            )
    if findings:
        print(f"check_number_format.py: {len(findings)} finding(s):", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1
    print(
        "check_number_format.py: every spec edge row has a probe, and every "
        "probe CHECK matches its row"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
