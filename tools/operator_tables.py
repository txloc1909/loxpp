"""Readers for the operator tables of spec/ and for the expression grammar.

check_int_operand_table.py, check_precedence_table.py, and
check_operator_protocol_table.py import this module. It reads the precedence
table and the expression rules of spec/02-syntax.md and states no rule of its
own.
"""

import re
from dataclasses import dataclass

from spec_tables import SpecError, section, table


@dataclass
class PrecedenceRow:
    number: int
    operators: list[str]
    associativity: str


def precedence_rows(syntax_text: str) -> list[PrecedenceRow]:
    """Returns the rows of the precedence table in spec/02-syntax.md."""
    rows = []
    for row in table(section(syntax_text, "## Operator Precedence"), "Precedence"):
        m = re.match(r"(\d+)", row["Precedence"])
        if not m:
            raise SpecError(f"precedence row has no number: {row['Precedence']!r}")
        rows.append(
            PrecedenceRow(
                int(m.group(1)),
                re.findall(r"`([^`]+)`", row["Operators"]),
                row["Associativity"],
            )
        )
    return rows


def unary_row(rows: list[PrecedenceRow]) -> PrecedenceRow:
    found = [r for r in rows if r.associativity == "Right (prefix)"]
    if len(found) != 1:
        raise SpecError("the precedence table must have exactly one prefix row")
    return found[0]


def binary_operators(rows: list[PrecedenceRow]) -> list[str]:
    """The operators of the rows between `and` and the prefix row, except `in`.

    `in` is a comparison-level operator that takes a sequence, not an
    operator on numbers, so the tables of the Int kind leave it out.
    """
    ops: list[str] = []
    seen_and = False
    for row in rows:
        if row.operators == ["and"]:
            seen_and = True
            continue
        if row.associativity == "Right (prefix)":
            break
        if seen_and:
            ops.extend(op for op in row.operators if op != "in")
    return ops


def operator_id(op: str, unary: bool) -> str:
    """The id of an operator in the tables of the Int kind: unary `-` is `neg`."""
    return "neg" if unary and op == "-" else op


def operand_expression_id(cell_code: str) -> str | None:
    """Maps the code of a table cell, such as `a + b`, `-a`, or `~a`, to the id
    of its operator. Returns None for a cell that is not an operator
    expression of one or two operands."""
    m = re.fullmatch(r"a (\S+) b", cell_code)
    if m:
        return m.group(1)
    m = re.fullmatch(r"-a", cell_code)
    if m:
        return "neg"
    m = re.fullmatch(r"([~!])a", cell_code)
    if m:
        return m.group(1)
    return None


def grammar_rules(syntax_text: str) -> dict[str, str]:
    """Returns the EBNF rules of spec/02-syntax.md as name -> body, with the
    comments removed."""
    start = syntax_text.index("## Complete EBNF Grammar")
    fence = syntax_text.index("```", start)
    block = syntax_text[fence + 3 : syntax_text.index("```", fence + 3)]
    block = re.sub(r"\(\*.*?\*\)", "", block, flags=re.S)
    rules: dict[str, str] = {}
    for m in re.finditer(r"^(\w+)\s*::=(.*?)(?=^\w+\s*::=|\Z)", block, flags=re.S | re.M):
        rules[m.group(1)] = m.group(2).strip()
    return rules


def expression_chain(rules: dict[str, str]) -> list[tuple[str, list[str], str]]:
    """Follows the expression rules from `logicOr` to `unary`.

    Returns one (rule, operators, associativity) for each rule, from the
    lowest precedence to the highest. A rule that is `operand ( "op" operand )*`
    is a binary rule: its associativity is Left. The `unary` rule, which starts
    with a group of prefix operators, is Right (prefix).
    """
    chain: list[tuple[str, list[str], str]] = []
    name = "logicOr"
    while True:
        if name not in rules:
            raise SpecError(f"the grammar has no rule {name!r}")
        body = rules[name]
        if body.startswith("("):
            group = re.match(r"\(([^)]*)\)", body)
            if not group:
                raise SpecError(f"rule {name!r}: no operator group")
            chain.append((name, re.findall(r'"([^"]+)"', group.group(1)), "Right (prefix)"))
            return chain
        m = re.match(r"(\w+)\s*\((.*)\)\*", body, flags=re.S)
        if not m:
            raise SpecError(f"rule {name!r} is not a binary rule: {body!r}")
        chain.append((name, re.findall(r'"([^"]+)"', m.group(2)), "Left"))
        name = m.group(1)
