#!/usr/bin/env python3
"""Checks the precedence table in spec/02-syntax.md, section "Operator
Precedence".

The table must agree with the expression rules of the grammar in the same
file. The rules, from `logicOr` to `unary`, give the order of the groups from
the lowest precedence to the highest, the operators of each group, and its
associativity: a rule of the form `operand ( "op" operand )*` is left
associative, and `unary` is a prefix rule. The table rows 2 to 12 must list
the same operators, in the same order, with the same associativity. Row 1 is
the assignment, and the last row is the call, the property access, and the
index.

Each row that holds a bit operator, and the prefix row, needs a spec probe: a
probe of test/translation-probes/ with a line

    // COVERS: precedence <row number>

That probe has an expression whose grouping under the row above or below
gives another value. A row with no probe fails, and so does a COVERS line that
names a row that needs none.

Usage:
    tools/check_precedence_table.py [--syntax PATH] [--probes DIR]
"""

import argparse
from pathlib import Path

from operator_tables import expression_chain, grammar_rules, precedence_rows
from spec_tables import PROBE_DIR, SpecError, probe_covers, report, spec_path

SCRIPT = "check_precedence_table.py"
# The operators that the Int kind adds (docs/decisions/integer-kind.md).
BIT_OPERATORS = {"|", "^", "&", "<<", ">>", ">>>", "~"}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--syntax", help="path to a copy of spec/02-syntax.md")
    parser.add_argument("--probes", help="a directory of spec probes", default=str(PROBE_DIR))
    args = parser.parse_args()

    problems: list[str] = []
    try:
        syntax = spec_path("02-syntax.md", args.syntax).read_text(encoding="utf-8")
        rows = precedence_rows(syntax)
        chain = expression_chain(grammar_rules(syntax))
        covers = probe_covers(Path(args.probes))
    except SpecError as exc:
        report(SCRIPT, [f"cannot read the spec: {exc}"], "")
        return

    numbers = [r.number for r in rows]
    if numbers != list(range(1, len(rows) + 1)):
        problems.append(f"the rows are numbered {numbers}, not 1 to {len(rows)}")
    if rows and rows[0].operators != ["="]:
        problems.append(f"row 1 lists {rows[0].operators}, not the assignment `=`")
    if rows and "()" not in rows[-1].operators:
        problems.append(f"the last row lists {rows[-1].operators}, not the call")
    if len(rows) != len(chain) + 2:
        problems.append(
            f"the grammar has {len(chain)} expression rules from logicOr to unary, so the "
            f"table needs {len(chain) + 2} rows, and it has {len(rows)}"
        )

    for offset, (rule, operators, assoc) in enumerate(chain):
        number = offset + 2
        if number > len(rows):
            break
        row = rows[number - 1]
        if set(row.operators) != set(operators):
            problems.append(
                f"row {number}: the table lists {sorted(row.operators)}, the grammar rule "
                f"{rule} lists {sorted(operators)}"
            )
        if row.associativity != assoc:
            problems.append(
                f"row {number}: the table says {row.associativity!r}, the grammar rule "
                f"{rule} is {assoc!r}"
            )

    needed = set()
    for row in rows:
        if BIT_OPERATORS & set(row.operators):
            needed.add(row.number)
    for number in sorted(needed):
        if ("precedence", str(number)) not in covers:
            problems.append(
                f"row {number} holds a bit operator and no spec probe has "
                f"`// COVERS: precedence {number}`"
            )
    for words in sorted(w for w in covers if w[0] == "precedence"):
        if len(words) != 2 or not words[1].isdigit() or int(words[1]) not in needed:
            problems.append(
                f"`// COVERS: {' '.join(words)}` in {covers[words][0].name} names no row "
                "that holds a bit operator"
            )

    report(
        SCRIPT,
        problems,
        f"{len(rows)} rows, {len(needed)} with a probe; the table matches the grammar",
    )


if __name__ == "__main__":
    main()
