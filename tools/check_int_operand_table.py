#!/usr/bin/env python3
"""Checks the operand-kind table in spec/04-semantics.md, section "Operand
Kinds".

The table has one row for each operator that takes numbers: every binary
operator of the precedence table in spec/02-syntax.md, except `in`, and every
prefix operator of it. A row removed here, or an operator added to the
precedence table with no row here, fails.

Each row has three cells, for two Ints, for two Numbers, and for an Int
together with a Number. A cell that says "as today" is the behavior that the
Number operators had before the Int existed, and the cell "—" is a case that
does not apply to a unary operator. Every other cell needs a spec probe: a
probe of test/translation-probes/ with a line

    // COVERS: operand-kind <operator> <column>

where <operator> is the operator (`neg` for unary `-`) and <column> is `int`,
`number`, or `mixed`. A cell with no probe fails, and so does a COVERS line
that names no cell that needs one.

Usage:
    tools/check_int_operand_table.py [--syntax PATH] [--semantics PATH]
                                     [--probes DIR]
"""

import argparse
from pathlib import Path

from operator_tables import (
    binary_operators,
    operand_expression_id,
    operator_id,
    precedence_rows,
    unary_row,
)
from spec_tables import (
    PROBE_DIR,
    SpecError,
    code,
    probe_covers,
    report,
    section,
    spec_path,
    table,
)

SCRIPT = "check_int_operand_table.py"
COLUMNS = {"int": "Int, Int", "number": "Number, Number", "mixed": "Int and Number"}


def needs_probe(cell: str) -> bool:
    return cell != "—" and "as today" not in cell.lower()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--syntax", help="path to a copy of spec/02-syntax.md")
    parser.add_argument("--semantics", help="path to a copy of spec/04-semantics.md")
    parser.add_argument("--probes", help="a directory of spec probes", default=str(PROBE_DIR))
    args = parser.parse_args()

    problems: list[str] = []
    try:
        syntax = spec_path("02-syntax.md", args.syntax).read_text(encoding="utf-8")
        semantics = spec_path("04-semantics.md", args.semantics).read_text(encoding="utf-8")
        rows = precedence_rows(syntax)
        expected = {operator_id(op, False) for op in binary_operators(rows)} | {
            operator_id(op, True) for op in unary_row(rows).operators
        }
        table_rows = table(section(semantics, "### Operand Kinds"), "Operation")
        covers = probe_covers(Path(args.probes))
    except SpecError as exc:
        report(SCRIPT, [f"cannot read the spec: {exc}"], "")
        return

    seen: dict[str, dict[str, str]] = {}
    for row in table_rows:
        try:
            op = operand_expression_id(code(row["Operation"]))
        except SpecError as exc:
            problems.append(str(exc))
            continue
        if op is None:
            problems.append(f"row {row['Operation']!r} is not one operator expression")
            continue
        if op in seen:
            problems.append(f"operator {op!r} has more than one row")
        seen[op] = row
        for column in COLUMNS.values():
            if not row[column]:
                problems.append(f"operator {op!r}: the cell {column!r} is empty")

    for op in sorted(expected - set(seen)):
        problems.append(f"operator {op!r} is in the precedence table but has no row")
    for op in sorted(set(seen) - expected):
        problems.append(f"operator {op!r} has a row but is not in the precedence table")

    needed = 0
    needed_cells = set()
    for op, row in seen.items():
        for key, column in COLUMNS.items():
            if not needs_probe(row[column]):
                continue
            needed += 1
            needed_cells.add(("operand-kind", op, key))
            if ("operand-kind", op, key) not in covers:
                problems.append(
                    f"operator {op!r}, column {column!r}: no spec probe has "
                    f"`// COVERS: operand-kind {op} {key}`"
                )
    for words in sorted(w for w in covers if w[0] == "operand-kind"):
        if words not in needed_cells:
            problems.append(
                f"`// COVERS: {' '.join(words)}` in "
                f"{covers[words][0].name} names no cell that needs a probe"
            )

    report(
        SCRIPT,
        problems,
        f"{len(seen)} operators, {needed} cells with a probe; "
        "the table matches the precedence table",
    )


if __name__ == "__main__":
    main()
