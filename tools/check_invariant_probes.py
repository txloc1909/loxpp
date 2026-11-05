#!/usr/bin/env python3
"""Checks that the invariant probes are connected to the spec and to the tests.

The table "Probes" in docs/decisions/concurrency-invariant-probes.md lists one
row for each case of the invariants I1, I2 and I3. The rules:

  - Each row of the task-owned state table in "The Task Machine" of
    spec/04-semantics.md has one I1 row, and no I1 row names another case.
  - Each point of the suspend point table of spec/04-semantics.md that is of
    class receive, or is `sleep`, a `Socket` call or a `Server` call, has one
    I2 row. The Case cell of such a row starts with that point.
  - A probe in the last column is a file of test/translation-probes/ (at any
    depth) with a `// STATUS:` line and a row in test/translation-probes/README.md.
  - A row whose last column is `—` has no probe yet. That passes unless
    --strict is given. --strict also needs one row of each of I1, I2 and I3.

A row removed from the table, a probe removed from the directory, and a probe
removed from the README each fail.

Usage:
    tools/check_invariant_probes.py [--strict] [--decision PATH] [--semantics PATH]
                                    [--probes DIR]
"""

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def table_rows(text, header_prefix):
    """The rows of the first table whose header row starts with header_prefix."""
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if line.startswith(header_prefix):
            rows = []
            for row in lines[i + 2:]:
                if not row.startswith("|"):
                    break
                rows.append([c.strip() for c in row.strip().strip("|").split("|")])
            return rows
    return None


def first_code(cell):
    m = re.search(r"`([^`]+)`", cell)
    return m.group(1) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--decision", default=str(ROOT / "docs/decisions/concurrency-invariant-probes.md"))
    ap.add_argument("--semantics", default=str(ROOT / "spec/04-semantics.md"))
    ap.add_argument("--probes", default=str(ROOT / "test/translation-probes"))
    args = ap.parse_args()

    errors = []
    decision = pathlib.Path(args.decision).read_text()
    semantics = pathlib.Path(args.semantics).read_text()
    probe_dir = pathlib.Path(args.probes)

    rows = table_rows(decision, "| Id | Case | Probes |")
    if rows is None:
        print("check_invariant_probes: no table '| Id | Case | Probes |' in " + args.decision)
        return 1
    owned = table_rows(semantics, "| Task-owned state |")
    points = table_rows(semantics, "| Point | Class |")
    if owned is None or points is None:
        print("check_invariant_probes: a spec table is missing")
        return 1

    by_id = {"I1": [], "I2": [], "I3": []}
    for row in rows:
        if len(row) != 3 or row[0] not in by_id:
            errors.append(f"bad row of the probe table: {row}")
            continue
        by_id[row[0]].append(row)

    want_i1 = [r[0] for r in owned]
    got_i1 = [r[1] for r in by_id["I1"]]
    for f in want_i1:
        if f not in got_i1:
            errors.append(f"no I1 row for the task-owned state '{f}'")
    for f in got_i1:
        if f not in want_i1:
            errors.append(f"I1 row '{f}' is not a task-owned state of the spec")

    want_i2 = []
    for p in points:
        point, cls = first_code(p[0]), p[1]
        if point and (cls == "receive" or point.startswith(("sleep(", "Socket.", "Server."))):
            want_i2.append(point)
    got_i2 = [first_code(r[1]) for r in by_id["I2"]]
    for pt in want_i2:
        if pt not in got_i2:
            errors.append(f"no I2 row for the suspend point '{pt}'")

    readme = (probe_dir / "README.md").read_text()
    files = {f.stem: f for f in probe_dir.rglob("*.lox")}
    for row in rows:
        if len(row) != 3 or row[0] not in by_id:
            continue
        names = re.findall(r"`([^`]+)`", row[2])
        if not names:
            if args.strict:
                errors.append(f"{row[0]} '{row[1]}' has no probe")
            continue
        for n in names:
            f = files.get(n)
            if f is None:
                errors.append(f"{row[0]} '{row[1]}': probe {n} is not a file of {probe_dir}")
                continue
            if not re.search(r"^// STATUS: (ok|error)\b", f.read_text(), re.M):
                errors.append(f"probe {n} has no STATUS line")
            if f"`{n}`" not in readme:
                errors.append(f"probe {n} has no row in {probe_dir / 'README.md'}")

    if args.strict:
        for i in by_id:
            if not any(re.search(r"`[^`]+`", r[2]) for r in by_id[i]):
                errors.append(f"{i} has no probe")

    for e in errors:
        print("check_invariant_probes: " + e)
    if errors:
        return 1
    print(f"check_invariant_probes: {len(rows)} rows, {len(want_i1)} I1 cases, "
          f"{len(want_i2)} I2 points checked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
