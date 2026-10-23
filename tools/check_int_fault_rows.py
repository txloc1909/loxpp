#!/usr/bin/env python3
"""Checks the run-time fault rows of the Int kind in spec/04-semantics.md and
spec/05-stdlib.md.

ROWS is the set that docs/decisions/integer-kind.md names (countable sets,
"the runtime fault rows"): a mixed-kind operand, a bit operator on a Number or
an Instance, a shift count that is not an Int or is out of range, Int `/ 0i`
and `% 0i`, `int()` on a fractional Number, a NaN, an infinity, a Number
outside the Int range, or a String that is not decimal, `float()` on a String
that does not parse, and each module function on a non-Int. Each row has an id
and an example. For each row this script checks four things, so a row removed
from any one of them fails:

 1. The spec table has a row whose Example cell holds the example: the
    catchable Runtime Errors table, with the `kind` of the row, or the Halting
    Faults table.
 2. tools/check_fault_table.py has a row of that name, with the disposition of
    the table and the kind of the row, and its program holds the example.
 3. A spec probe of test/translation-probes/ has a line
        // COVERS: fault <id>
    A probe of a catchable fault has `// STATUS: ok`, because it catches the
    fault. A probe of a halting fault has `// STATUS: error`.
 4. No COVERS line names an id that is not in ROWS.

Two rows hold one fault per function of the `integer` module, so one probe is
not enough for them:

    integer_module_non_int              every function of the module
    integer_unsigned_division_by_zero   every function whose spec row says that
                                        a zero divisor is a runtime error

For these rows the probe line names the function:

    // COVERS: fault integer_module_non_int urem
    // COVERS: fault integer_unsigned_division_by_zero urem

The function names come from the member table of `integer` in
spec/05-stdlib.md (a function is a member written `integer.<name>(`). The
decision names eight functions, MODULE_FUNCTIONS, and two of them divide,
DIVIDING_FUNCTIONS. A function in the member table with no probe fails, so
does a function of the decision that the table lost, and so does a probe that
names a function that its line of the fault does not call.

Such a probe has the shape of the probes 221 to 233: a control line that
calls the function on valid arguments, the line of the fault, and the line
`print "unreachable";`. The line of the fault is the last line of code before
the `print`. It must call the function that the COVERS line names, and it must
not repeat the control line, because a call that repeats a call which runs
does not fault. The control line calls the function too, so a check of every
line of code could not see a change to the line of the fault alone.

Usage:
    tools/check_int_fault_rows.py [--semantics PATH] [--stdlib PATH] [--probes DIR]
"""

import argparse
import re
from dataclasses import dataclass
from pathlib import Path

from check_spec_probes import parse_probe
from spec_tables import PROBE_DIR, SpecError, probe_covers, report, section, spec_path, table

SCRIPT = "check_int_fault_rows.py"


# The functions of the `integer` module, as docs/decisions/integer-kind.md
# lists them, and the two of them that divide. The member table of the spec is
# the source of the names; these sets say what the decision requires of it.
MODULE_FUNCTIONS = {"abs", "min", "max", "udiv", "urem", "ult", "ustr", "parse_unsigned"}
DIVIDING_FUNCTIONS = {"udiv", "urem"}
INTEGER_HEADING = "## `integer` — Int functions and constants"

EVERY_FUNCTION = "every function"
DIVIDING = "every dividing function"


@dataclass
class FaultRow:
    id: str
    kind: str | None  # the Error.kind of a catchable fault, None for a halting one
    example: str  # appears in the Example cell of the spec table
    per_function: str | None = None  # EVERY_FUNCTION or DIVIDING: one probe for each function


CATCHABLE = "catchable"
HALTING = "halting"

ROWS = [
    # A mixed-kind operand.
    FaultRow("arithmetic_type_error_mixed_int", "ArithmeticTypeError", "1i - 1"),
    FaultRow("comparison_type_error_mixed_int", "ComparisonTypeError", "1i < 1"),
    FaultRow("concatenation_type_error_mixed_int", "ConcatenationTypeError", "1i + 1"),
    # A bit operator on a Number or an Instance, and a shift count that is not an Int.
    FaultRow("bitwise_type_error", "BitwiseTypeError", "1i & 1"),
    FaultRow("bitwise_type_error_instance", "BitwiseTypeError", "class C {} C() & 1i"),
    FaultRow("bitwise_type_error_shift_count", "BitwiseTypeError", "1i << 1"),
    # A shift count out of range.
    FaultRow("shift_count_error", "ShiftCountError", "1i << 64i"),
    # Int `/ 0i` and `% 0i`.
    FaultRow("division_by_zero_error", "DivisionByZeroError", "1i / 0i"),
    FaultRow("division_by_zero_error_modulo", "DivisionByZeroError", "1i % 0i"),
    # The result check of an operator method does not change.
    FaultRow(
        "operator_result_type_error_int",
        "OperatorResultTypeError",
        "class C { __len__() { return 1i; } } len(C())",
    ),
    # int()
    FaultRow("int_fractional", None, "int(1.5);"),
    FaultRow("int_nan", None, "int(math.nan);"),
    FaultRow("int_infinity", None, "int(math.inf);"),
    FaultRow("int_out_of_range", None, "int(9223372036854775808.0);"),
    FaultRow("int_string_not_decimal", None, 'int("1.5");'),
    FaultRow("int_other_kind", None, "int(nil);"),
    # float()
    FaultRow("float_string_not_number", None, 'float("abc");'),
    FaultRow("float_other_kind", None, "float(nil);"),
    # the integer module
    FaultRow("integer_module_non_int", None, "integer.abs(1);", EVERY_FUNCTION),
    FaultRow("integer_unsigned_division_by_zero", None, "integer.udiv(1i, 0i);", DIVIDING),
    FaultRow(
        "integer_parse_unsigned_invalid",
        None,
        'integer.parse_unsigned("18446744073709551616");',
    ),
]


def squash(text: str) -> str:
    return re.sub(r"\s+", " ", text).strip()


UNREACHABLE = 'print "unreachable";'


def code_lines(path: Path) -> list[str]:
    return [l.strip() for l in path.read_text(encoding="utf-8").splitlines() if l.strip() and not l.lstrip().startswith("//")]


def fault_line(lines: list[str]) -> tuple[str, list[str]] | None:
    """Returns the line of the fault, the last line of code before the line
    that prints "unreachable", and the lines of code before it. None when the
    probe has no such line, or has no line before it."""
    if UNREACHABLE not in lines:
        return None
    at = lines.index(UNREACHABLE)
    return (lines[at - 1], lines[: at - 1]) if at else None


def statement(line: str) -> str:
    """Returns a line of code without a leading `print` and the final `;`."""
    return line.removeprefix("print ").removesuffix(";").strip()


def module_functions(stdlib: str) -> tuple[set[str], set[str]]:
    """Returns the functions of the member table of `integer`, and those whose
    Meaning cell says that a zero divisor is a runtime error."""
    functions: set[str] = set()
    dividing: set[str] = set()
    for member in table(section(stdlib, INTEGER_HEADING), "Member"):
        names = set(re.findall(r"integer\.(\w+)\(", member["Member"]))
        functions |= names
        if "zero" in member["Meaning"].lower():
            dividing |= names
    return functions, dividing


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--semantics", help="path to a copy of spec/04-semantics.md")
    parser.add_argument("--stdlib", help="path to a copy of spec/05-stdlib.md")
    parser.add_argument("--probes", help="a directory of spec probes", default=str(PROBE_DIR))
    args = parser.parse_args()

    problems: list[str] = []
    try:
        semantics = spec_path("04-semantics.md", args.semantics).read_text(encoding="utf-8")
        catchable_rows = table(section(semantics, "## Runtime Errors"), "Cause")
        halting_rows = table(section(semantics, "### Halting Faults"), "Cause")
        functions, dividing = module_functions(spec_path("05-stdlib.md", args.stdlib).read_text(encoding="utf-8"))
        covers = probe_covers(Path(args.probes))
        import check_fault_table as cft

        fault_rows = {r.name: r for r in cft.ALL_ROWS}
    except (SpecError, ImportError) as exc:
        report(SCRIPT, [f"cannot read the inputs: {exc}"], "")
        return

    ids = [r.id for r in ROWS]
    if len(set(ids)) != len(ids):
        problems.append("ROWS holds an id twice")

    for name in sorted(MODULE_FUNCTIONS - functions):
        problems.append(f"the member table of `integer` has no function {name!r}, which the decision names")
    for name in sorted(DIVIDING_FUNCTIONS - dividing):
        problems.append(f"the member table of `integer` does not say that {name!r} faults on a zero divisor")
    for name in sorted(dividing - DIVIDING_FUNCTIONS):
        problems.append(
            f"the member table of `integer` says that {name!r} faults on a zero divisor, "
            f"and DIVIDING_FUNCTIONS does not name it"
        )
    scope = {EVERY_FUNCTION: functions | MODULE_FUNCTIONS, DIVIDING: dividing | DIVIDING_FUNCTIONS}
    known: set[tuple[str, ...]] = set()

    for row in ROWS:
        halting = row.kind is None
        # 1. the spec table
        found = [
            r
            for r in (halting_rows if halting else catchable_rows)
            if row.example in r["Example"]
        ]
        if not found:
            problems.append(
                f"{row.id}: the {'Halting Faults' if halting else 'Runtime Errors'} table has no "
                f"row whose example holds {row.example!r}"
            )
        elif not halting and not any(f'"{row.kind}"' in r["`Error.kind`"] for r in found):
            problems.append(f"{row.id}: the spec row of {row.example!r} does not have the kind {row.kind}")
        # 2. tools/check_fault_table.py
        fault = fault_rows.get(row.id)
        if fault is None:
            problems.append(f"{row.id}: tools/check_fault_table.py has no row of that name")
        else:
            want = "fatal" if halting else "caught"
            if fault.disposition != want:
                problems.append(f"{row.id}: the fault table row is {fault.disposition!r}, not {want!r}")
            if not halting and fault.expected_kind != row.kind:
                problems.append(f"{row.id}: the fault table row has the kind {fault.expected_kind}, not {row.kind}")
            program = squash(fault.setup + " " + fault.body)
            if squash(row.example).rstrip(";") not in program:
                problems.append(f"{row.id}: the fault table program does not hold the example {row.example!r}")
        # 3. the probes: one COVERS line, or one for each function of the module
        if row.per_function:
            words_of_row = [(("fault", row.id, name), name) for name in sorted(scope[row.per_function])]
        else:
            words_of_row = [(("fault", row.id), None)]
        for words, name in words_of_row:
            known.add(words)
            paths = covers.get(words, [])
            if not paths:
                problems.append(f"{row.id}: no spec probe has `// COVERS: {' '.join(words)}`")
            for path in paths:
                probe = parse_probe(path)
                want_status = "error" if halting else "ok"
                if probe is None or probe.status != want_status:
                    problems.append(
                        f"{path.name} covers {row.id}: a probe of a "
                        f"{'halting' if halting else 'catchable'} fault needs `// STATUS: {want_status}`"
                    )
                if name:
                    found_fault = fault_line(code_lines(path))
                    if found_fault is None:
                        problems.append(
                            f"{path.name} covers {row.id} for {name!r}: it has no line of the fault "
                            f"before `{UNREACHABLE}`"
                        )
                        continue
                    line, before = found_fault
                    if not line.startswith(f"integer.{name}("):
                        problems.append(
                            f"{path.name} covers {row.id} for {name!r}: its line of the fault, "
                            f"`{line}`, does not call integer.{name}("
                        )
                    elif statement(line) in [statement(b) for b in before]:
                        problems.append(
                            f"{path.name} covers {row.id} for {name!r}: its line of the fault, "
                            f"`{line}`, repeats a line that runs before it, so it does not fault"
                        )

    for words in sorted(w for w in covers if w[0] == "fault"):
        if words not in known:
            problems.append(f"`// COVERS: {' '.join(words)}` in {covers[words][0].name} names no row")

    report(
        SCRIPT,
        problems,
        f"{len(ROWS)} rows ({sum(1 for r in ROWS if r.kind)} catchable, "
        f"{sum(1 for r in ROWS if not r.kind)} halting), each in the spec, in the fault table "
        f"check, and in a probe; {len(scope[EVERY_FUNCTION])} functions of the `integer` module "
        f"each with a non-Int probe, {len(scope[DIVIDING])} of them each with a zero-divisor probe",
    )


if __name__ == "__main__":
    main()
