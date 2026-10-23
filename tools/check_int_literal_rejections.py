#!/usr/bin/env python3
"""Checks the static errors and the examples of Int literals in
spec/01-lexical.md, sections "Static errors in Int literals" and "Examples of
the literal rules".

The first table must have one row for each static error that
docs/decisions/integer-kind.md names (countable sets): a decimal literal
outside the Int range, a hexadecimal or a binary literal of more than 64 bits,
a fractional literal with the suffix, and a name directly after a literal. A
row removed here fails.

Each row needs two kinds of spec probe in test/translation-probes/:

    // COVERS: literal <row>           a probe of a program that is rejected
    // COVERS: literal-accepts <row>   a probe of a program at the accepted limit

The first has `// STATIC-ERROR` and `// STATUS: error`. The second has
`// STATUS: ok`. The second is the twin of the first: an implementation that
rejects every Int literal passes the first and fails the second, so the pair
tells a rejection for the right reason from a rejection by accident.

Each rejected probe starts with a line that prints a String. A static error
rejects the program, so nothing prints. An implementation that finds the error
only when it runs the program prints that String, and the differential gates
and tools/check_spec_probes.py see the difference. A rejected probe with no
such line cannot tell a rejection at compile time from a halt at run time.

The second table lists the forms that follow from the three rules of the
section: the longest match, the range, and the unary-minus exception. It is the
only list of forms. Each row has a `Source`, the `Tokens` that the longest
match reads, and a `Result`: `valid`, or the row of the first table that the
program breaks. For each row this script needs a spec probe with a line of code
that holds the `Source`: the line is the `Source`, or `print <Source>;`, or
a match arm that starts with the `Source` and `=>`. The whole line must match,
so `9223372036854775808i` is not found in the line of `-9223372036854775808i`.
Then:

 - A `valid` row needs a probe with `// STATUS: ok` and no `// STATIC-ERROR`,
   and no probe with `// STATIC-ERROR` may hold its `Source`.
 - A row with a row id needs a probe with `// STATIC-ERROR` and
   `// COVERS: literal <row id>`, and each probe that holds its `Source` must
   have both.

The script does not read the `Tokens` column: it is for the reader.

Each cell of the first table that is one code span is a form of the second. The
`Example` cell is a row whose `Result` is the row of its own, and the
`Accepted limit` cell is a row whose `Result` is `valid`. A cell of prose, such
as `0b` followed by 64 ones, is skipped.

The check also runs the other way. Each rejected probe holds the `Source` of a
row whose `Result` is the row of its COVERS line, so a form that a probe
rejects cannot be missing from the table. The `valid` rows have no such link,
because the probes that hold them hold other lines too (every line of probe 165
prints a literal). The count VALID_EXAMPLES says how many `valid` rows the table
has, so a `valid` row removed from the table fails. A row added on purpose
needs a new count in this script.

Usage:
    tools/check_int_literal_rejections.py [--lexical PATH] [--probes DIR]
"""

import argparse
import re
from pathlib import Path

from check_spec_probes import parse_probe
from spec_tables import PROBE_DIR, SpecError, code, probe_covers, report, section, spec_path, table

SCRIPT = "check_int_literal_rejections.py"
# The rows of docs/decisions/integer-kind.md, "Countable sets", with the hex
# and the binary row apart, and the suffix boundary.
EXPECTED_ROWS = {"decimal-range", "hex-width", "binary-width", "fractional-suffix", "suffix-boundary"}
# The `valid` rows of the table of examples in spec/01-lexical.md.
VALID_EXAMPLES = 19
VALID = "valid"
# The first line of code of a rejected probe: a `print` of a String.
LEADING_PRINT = re.compile(r'^print "[^"\\]+";$')


def code_lines(path: Path) -> list[str]:
    return [
        line.strip()
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("//")
    ]


def is_static(path: Path) -> bool:
    return any(l.strip() == "// STATIC-ERROR" for l in path.read_text(encoding="utf-8").splitlines())


def holds(line: str, source: str) -> bool:
    """True when the line of code is the `Source`, a `print` of it, or a match
    arm that starts with it."""
    return line == source or line == f"print {source};" or line.startswith(f"{source} =>")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--lexical", help="path to a copy of spec/01-lexical.md")
    parser.add_argument("--probes", help="a directory of spec probes", default=str(PROBE_DIR))
    args = parser.parse_args()

    problems: list[str] = []
    probe_dir = Path(args.probes)
    try:
        lexical = spec_path("01-lexical.md", args.lexical).read_text(encoding="utf-8")
        rows = table(section(lexical, "### Static errors in Int literals"), "Row")
        examples = table(section(lexical, "### Examples of the literal rules"), "Source")
        covers = probe_covers(probe_dir)
        probes = [p for p in sorted(probe_dir.glob("*.lox")) if parse_probe(p) is not None]
        lines_of = {p: code_lines(p) for p in probes}
        covers_of: dict[Path, set[tuple[str, ...]]] = {}
        for words, paths in covers.items():
            for path in paths:
                covers_of.setdefault(path, set()).add(words)
    except SpecError as exc:
        report(SCRIPT, [f"cannot read the spec: {exc}"], "")
        return

    ids = []
    for row in rows:
        try:
            ids.append(code(row["Row"]))
        except SpecError as exc:
            problems.append(str(exc))
    for row_id in sorted(set(ids) - set(EXPECTED_ROWS)):
        problems.append(f"row {row_id!r} is not a static error that the decision names")
    for row_id in sorted(EXPECTED_ROWS - set(ids)):
        problems.append(f"the static error {row_id!r} has no row")
    for row_id in sorted({i for i in ids if ids.count(i) > 1}):
        problems.append(f"row {row_id!r} appears more than once")

    for row_id in sorted(EXPECTED_ROWS):
        rejecting = covers.get(("literal", row_id), [])
        accepting = covers.get(("literal-accepts", row_id), [])
        if not rejecting:
            problems.append(f"row {row_id!r}: no spec probe has `// COVERS: literal {row_id}`")
        if not accepting:
            problems.append(f"row {row_id!r}: no spec probe has `// COVERS: literal-accepts {row_id}`")
        for path in rejecting:
            probe = parse_probe(path)
            if not is_static(path) or probe is None or probe.status != "error":
                problems.append(f"{path.name} rejects row {row_id!r}: it needs `// STATIC-ERROR` and `// STATUS: error`")
        for path in accepting:
            probe = parse_probe(path)
            if probe is None or probe.status != "ok" or is_static(path):
                problems.append(f"{path.name} accepts row {row_id!r}: it needs `// STATUS: ok` and no `// STATIC-ERROR`")
    rejected_paths = sorted(set(sum((covers.get(("literal", r), []) for r in EXPECTED_ROWS), [])))
    for path in rejected_paths:
        first = code_lines(path)[:1]
        if not first or not LEADING_PRINT.match(first[0]):
            problems.append(
                f"{path.name} is a rejected probe: its first line of code must print a String "
                f"(`print \"ran\";`), so that an implementation that runs the program "
                f"before it finds the error prints something"
            )
    for words in sorted(w for w in covers if w[0] in ("literal", "literal-accepts")):
        if len(words) != 2 or words[1] not in EXPECTED_ROWS:
            problems.append(f"`// COVERS: {' '.join(words)}` in {covers[words][0].name} names no row")

    # The table of examples, row by row.
    sources: list[str] = []
    forms: set[tuple[str, str]] = set()  # (Source, Result) of each row
    valid_rows = 0
    held_by: dict[Path, set[str]] = {}  # a rejected probe -> the Results of the rows it holds
    for example in examples:
        try:
            source = code(example["Source"])
        except SpecError as exc:
            problems.append(f"examples: {exc}")
            continue
        sources.append(source)
        result = example["Result"].strip("`")
        forms.add((source, result))
        if not example["Tokens"].strip():
            problems.append(f"examples: the row of {source!r} has no tokens")
        if result != VALID and result not in EXPECTED_ROWS:
            problems.append(f"examples: the Result of {source!r} is {result!r}, which is not `valid` and not a row")
            continue
        if result == VALID:
            valid_rows += 1
        holders = [p for p in probes if any(holds(line, source) for line in lines_of[p])]
        if not holders:
            problems.append(f"examples: no spec probe has a line of code that holds the Source {source!r}")
            continue
        for path in holders:
            if is_static(path):
                held_by.setdefault(path, set()).add(result)
        if result == VALID:
            for path in holders:
                if is_static(path):
                    problems.append(
                        f"examples: the row of {source!r} says valid, and {path.name} holds it with `// STATIC-ERROR`"
                    )
            if not any(parse_probe(p).status == "ok" and not is_static(p) for p in holders):
                problems.append(f"examples: the row of {source!r} says valid, and no probe with `// STATUS: ok` holds it")
        else:
            for path in holders:
                if not is_static(path) or ("literal", result) not in covers_of.get(path, set()):
                    problems.append(
                        f"examples: the row of {source!r} says {result!r}, and {path.name} holds it "
                        f"without `// STATIC-ERROR` and `// COVERS: literal {result}`"
                    )
    for source in sorted({s for s in sources if sources.count(s) > 1}):
        problems.append(f"examples: the Source {source!r} appears more than once")
    for row_id in sorted(EXPECTED_ROWS):
        if not any(e["Result"].strip("`") == row_id for e in examples):
            problems.append(f"examples: no row has the Result {row_id!r}")
    if valid_rows != VALID_EXAMPLES:
        problems.append(
            f"examples: the table has {valid_rows} `valid` rows and this script expects {VALID_EXAMPLES}; "
            f"a row was removed, or a row was added and VALID_EXAMPLES is not new"
        )
    for path in rejected_paths:
        row_ids = {w[1] for w in covers_of.get(path, set()) if w[0] == "literal"}
        if not (held_by.get(path, set()) & row_ids):
            problems.append(
                f"{path.name} is a rejected probe of {', '.join(sorted(row_ids))}: "
                f"no row of the table of examples has a Source that it holds, with that Result"
            )
    # The Example cell and the Accepted limit cell of the first table are forms
    # of the second: the Example breaks the row, and the Accepted limit is valid.
    for row in rows:
        try:
            row_id = code(row["Row"])
        except SpecError:
            continue  # already reported with the row ids above
        for column, result in (("Example", row_id), ("Accepted limit", VALID)):
            try:
                cell_source = code(row[column])
            except SpecError:
                continue  # a cell of prose, such as `0b1` followed by 64 zeros
            if (cell_source, result) not in forms:
                problems.append(
                    f"row {row_id!r}: its {column} {cell_source!r} is not a row of the table of examples "
                    f"with the Result {result!r}"
                )

    report(
        SCRIPT,
        problems,
        f"{len(ids)} rows, each with a rejected probe and an accepted twin, "
        f"{len(rejected_paths)} rejected probes that print before the error, "
        f"and {len(examples)} examples ({valid_rows} valid), each held by a spec probe",
    )


if __name__ == "__main__":
    main()
