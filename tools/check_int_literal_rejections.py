#!/usr/bin/env python3
"""Checks the static errors of Int literals in spec/01-lexical.md, section
"Static errors in Int literals".

The table must have one row for each static error that
docs/decisions/integer-kind.md names (countable sets): a decimal literal
outside the Int range, a hexadecimal or a binary literal of more than 64 bits,
and a fractional literal with the suffix. A row removed here fails.

Each row needs two kinds of spec probe in test/translation-probes/:

    // COVERS: literal <row>           a probe of a program that is rejected
    // COVERS: literal-accepts <row>   a probe of a program at the accepted limit

The first has `// STATIC-ERROR` and `// STATUS: error`. The second has
`// STATUS: ok`. The second is the twin of the first: an implementation that
rejects every Int literal passes the first and fails the second, so the pair
tells a rejection for the right reason from a rejection by accident.

The unary-minus exception has its own forms, each of which a probe must hold:
the smallest Int as `-9223372036854775808i`, with and without a space, in a
probe that is accepted, and three forms that are rejected, in probes that carry
`// STATIC-ERROR`.

Usage:
    tools/check_int_literal_rejections.py [--lexical PATH] [--probes DIR]
"""

import argparse
from pathlib import Path

from check_spec_probes import parse_probe
from spec_tables import PROBE_DIR, SpecError, code, probe_covers, report, section, spec_path, table

SCRIPT = "check_int_literal_rejections.py"
# The rows of docs/decisions/integer-kind.md, "Countable sets", with the hex
# and the binary row apart.
EXPECTED_ROWS = {"decimal-range", "hex-width", "binary-width", "fractional-suffix"}
ACCEPTED_FORMS = ["-9223372036854775808i", "- 9223372036854775808i"]
REJECTED_FORMS = [
    "-(9223372036854775808i)",
    "0i - 9223372036854775808i",
    "-9223372036854775809i",
]


def code_lines(path: Path) -> list[str]:
    return [
        line.strip()
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("//")
    ]


def is_static(path: Path) -> bool:
    return any(l.strip() == "// STATIC-ERROR" for l in path.read_text(encoding="utf-8").splitlines())


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
        covers = probe_covers(probe_dir)
        exception_paragraph = "unary-minus exception" in lexical
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
    if not exception_paragraph:
        problems.append("the section no longer states the unary-minus exception")

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
    for words in sorted(w for w in covers if w[0] in ("literal", "literal-accepts")):
        if len(words) != 2 or words[1] not in EXPECTED_ROWS:
            problems.append(f"`// COVERS: {' '.join(words)}` in {covers[words][0].name} names no row")

    accepted_text = [l for p in sorted(set(sum((covers.get(("literal-accepts", r), []) for r in EXPECTED_ROWS), []))) for l in code_lines(p)]
    rejected_text = [
        l
        for p in sorted(set(sum((covers.get(("literal", r), []) for r in EXPECTED_ROWS), [])))
        for l in code_lines(p)
    ]
    for form in ACCEPTED_FORMS:
        if not any(form in line for line in accepted_text):
            problems.append(f"no accepted probe holds the form {form!r}")
    for form in REJECTED_FORMS:
        if not any(form in line for line in rejected_text):
            problems.append(f"no rejected probe holds the form {form!r}")

    report(
        SCRIPT,
        problems,
        f"{len(ids)} rows, each with a rejected probe and an accepted twin, "
        f"and the {len(ACCEPTED_FORMS) + len(REJECTED_FORMS)} forms of the unary-minus exception",
    )


if __name__ == "__main__":
    main()
