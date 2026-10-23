#!/usr/bin/env python3
"""Checks the Int edge rows in spec/04-semantics.md, section "Int Edge Cases".

The table must have one row for each case that docs/decisions/integer-kind.md
lists as defined and not a fault (countable sets): the smallest Int divided by
`-1i` and modulo `-1i`, the negation of the smallest Int, the largest Int plus
`1i`, `1 == 1i`, and a truthy `0i`. A row removed here fails.

Each row has an Example, a `print` statement, and a Result, the text that the
statement prints. A spec probe of test/translation-probes/ with a line

    // COVERS: edge <row>

must hold the Example as one of its `print` statements, and the CHECK line
of that statement must be the Result. The probe must print one line for each
`print` statement, in order, so the n-th statement and the n-th CHECK line
belong together. A row with no probe fails, and so does a Result that the probe
does not print.

Usage:
    tools/check_int_edge_rows.py [--semantics PATH] [--probes DIR]
"""

import argparse
from pathlib import Path

from check_spec_probes import parse_probe
from spec_tables import PROBE_DIR, SpecError, code, probe_covers, report, section, spec_path, table

SCRIPT = "check_int_edge_rows.py"
# The rows of docs/decisions/integer-kind.md, "Countable sets".
EXPECTED_ROWS = {
    "min-div-neg1",
    "min-mod-neg1",
    "neg-min",
    "max-plus-1",
    "int-number-equal",
    "zero-truthy",
}


def print_statements(path: Path) -> list[str]:
    return [
        line.strip()
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip().startswith("print ")
    ]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--semantics", help="path to a copy of spec/04-semantics.md")
    parser.add_argument("--probes", help="a directory of spec probes", default=str(PROBE_DIR))
    args = parser.parse_args()

    problems: list[str] = []
    try:
        semantics = spec_path("04-semantics.md", args.semantics).read_text(encoding="utf-8")
        rows = table(section(semantics, "### Int Edge Cases"), "Row")
        covers = probe_covers(Path(args.probes))
    except SpecError as exc:
        report(SCRIPT, [f"cannot read the spec: {exc}"], "")
        return

    seen: dict[str, dict[str, str]] = {}
    for row in rows:
        try:
            row_id = code(row["Row"])
            row["Example"] = code(row["Example"])
            row["Result"] = code(row["Result"])
        except SpecError as exc:
            problems.append(str(exc))
            continue
        if row_id in seen:
            problems.append(f"row {row_id!r} appears more than once")
        seen[row_id] = row
    for row_id in sorted(EXPECTED_ROWS - set(seen)):
        problems.append(f"the edge case {row_id!r} has no row")
    for row_id in sorted(set(seen) - EXPECTED_ROWS):
        problems.append(f"row {row_id!r} is not an edge case that the decision names")

    for row_id, row in sorted(seen.items()):
        paths = covers.get(("edge", row_id), [])
        if not paths:
            problems.append(f"row {row_id!r}: no spec probe has `// COVERS: edge {row_id}`")
            continue
        for path in paths:
            probe = parse_probe(path)
            statements = print_statements(path)
            if probe is None or probe.status != "ok":
                problems.append(f"{path.name} covers {row_id!r}: it needs `// STATUS: ok`")
                continue
            if len(statements) != len(probe.checks):
                problems.append(
                    f"{path.name} has {len(statements)} print statements and "
                    f"{len(probe.checks)} CHECK lines; they must match one to one"
                )
                continue
            if row["Example"] not in statements:
                problems.append(f"{path.name} does not hold the example {row['Example']!r} of row {row_id!r}")
                continue
            printed = probe.checks[statements.index(row["Example"])]
            if printed != row["Result"]:
                problems.append(
                    f"{path.name}: the probe expects {printed!r} for {row['Example']!r}, "
                    f"row {row_id!r} says {row['Result']!r}"
                )
    for words in sorted(w for w in covers if w[0] == "edge"):
        if len(words) != 2 or words[1] not in EXPECTED_ROWS:
            problems.append(f"`// COVERS: {' '.join(words)}` in {covers[words][0].name} names no row")

    report(
        SCRIPT,
        problems,
        f"{len(seen)} edge rows, each with a probe that prints the result of the table",
    )


if __name__ == "__main__":
    main()
