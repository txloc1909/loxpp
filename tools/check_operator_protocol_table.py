#!/usr/bin/env python3
"""Checks the operator protocol table in spec/04-semantics.md, section
"Operator Overloading".

Every operator of the precedence table in spec/02-syntax.md must be in the
protocol table, with the method that an Instance defines for it, or in the row
that says `none`. The operators that have no row of their own are the ones
that the spec derives from another: `!=`, `<=`, and `>=`, which `==`, `>`, and
`<` give, and `!`, which reads truthiness. The row that says `none` must hold
exactly the bit operators: the operators of the grammar rules `bitOr`,
`bitXor`, `bitAnd`, and `shift`, and `~`. A bit operator removed from that row
fails, and so does a bit operator that has a method.

Each bit operator needs a spec probe, a probe of test/translation-probes/ with
a line

    // COVERS: protocol <operator>

that shows an Instance operand is a fault. The result check of an operator
method does not change for an Int, so the methods `__len__` and `__hash__`
need a probe with a line

    // COVERS: protocol-result <method>

Usage:
    tools/check_operator_protocol_table.py [--syntax PATH] [--semantics PATH]
                                           [--probes DIR]
"""

import argparse
import re
from pathlib import Path

from operator_tables import (
    binary_operators,
    expression_chain,
    grammar_rules,
    operand_expression_id,
    operator_id,
    precedence_rows,
    unary_row,
)
from spec_tables import (
    PROBE_DIR,
    SpecError,
    probe_covers,
    report,
    section,
    spec_path,
    table,
)

SCRIPT = "check_operator_protocol_table.py"
# Operators that no row of the protocol table needs: the spec derives them.
DERIVED = {"!=", "<=", ">=", "!"}
RESULT_METHODS = ["__len__", "__hash__"]


def operator_of_span(span: str) -> str | None:
    """The operator that a code span of the table's first cell stands for."""
    op = operand_expression_id(span)
    if op is not None:
        return op
    m = re.fullmatch(r"x (in) c", span)
    return m.group(1) if m else None


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
        precedence_ids = (
            {operator_id(op, False) for op in binary_operators(rows)}
            | {operator_id(op, True) for op in unary_row(rows).operators}
            | {"in"}
        )
        chain = {rule: ops for rule, ops, _ in expression_chain(grammar_rules(syntax))}
        bit = set()
        for rule in ("bitOr", "bitXor", "bitAnd", "shift"):
            if rule not in chain:
                raise SpecError(f"the grammar has no expression rule {rule!r}")
            bit |= set(chain[rule])
        bit.add("~")
        overloading = section(semantics, "### Operator Overloading")
        protocol_rows = table(overloading, "Operator")
        covers = probe_covers(Path(args.probes))
    except SpecError as exc:
        report(SCRIPT, [f"cannot read the spec: {exc}"], "")
        return

    with_method: set[str] = set()
    none_ops: set[str] = set()
    none_rows = 0
    for row in protocol_rows:
        spans = re.findall(r"`([^`]+)`", row["Operator"])
        ops = {o for o in map(operator_of_span, spans) if o is not None}
        if row["Method"].startswith("none"):
            none_rows += 1
            none_ops |= ops
        else:
            with_method |= ops
    if none_rows != 1:
        problems.append(f"the table needs exactly one row that says `none`, and it has {none_rows}")

    for op in sorted(bit - none_ops):
        problems.append(f"bit operator {op!r} is not in the row that says `none`")
    for op in sorted(none_ops - bit):
        problems.append(f"operator {op!r} is in the row that says `none` but is not a bit operator")
    for op in sorted(bit & with_method):
        problems.append(f"bit operator {op!r} has a method in the table")
    for op in sorted(precedence_ids - DERIVED - with_method - none_ops):
        problems.append(f"operator {op!r} is in the precedence table but is not in the protocol table")

    for op in sorted(bit):
        if ("protocol", op) not in covers:
            problems.append(f"bit operator {op!r}: no spec probe has `// COVERS: protocol {op}`")
    for method in RESULT_METHODS:
        if ("protocol-result", method) not in covers:
            problems.append(
                f"method {method}: no spec probe has `// COVERS: protocol-result {method}`"
            )
    known = {("protocol", op) for op in bit} | {("protocol-result", m) for m in RESULT_METHODS}
    for words in sorted(w for w in covers if w[0] in ("protocol", "protocol-result")):
        if words not in known:
            problems.append(f"`// COVERS: {' '.join(words)}` in {covers[words][0].name} names no row")

    report(
        SCRIPT,
        problems,
        f"{len(bit)} bit operators in the `none` row, {len(with_method)} operators with a "
        "method; the table matches the precedence table",
    )


if __name__ == "__main__":
    main()
