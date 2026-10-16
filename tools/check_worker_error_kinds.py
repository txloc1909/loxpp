#!/usr/bin/env python3
"""Checks the Worker and Task Faults table in spec/04-semantics.md.

The rule sentences of the section "Workers and Tasks" name each fault:
"raises `K`" names a catchable fault, and "ends with `K`" names a fault that
ends the program. The `task` section of spec/05-stdlib.md uses the same
"raises `K`" form. The table must have exactly one row for each kind those
sentences name, and its Catchable column must agree with the sentence form.
A row removed from the table fails, and so does a kind that the sentences
name and the table does not.

A pre-existing kind that the section cites for another construct is written
in another form ("is a `RunningCoroutineError`"), so it is not one of the
faults this table counts.

Usage:
    tools/check_worker_error_kinds.py [--semantics PATH] [--stdlib PATH]
"""

import argparse
import re

from spec_tables import SpecError, code, report, section, spec_path, table

RAISES = re.compile(r"\braises\s+`(\w+)`")
ENDS_WITH = re.compile(r"\bends with\s+`(\w+)`")


def prose(body: str) -> str:
    """Returns the body without its table rows, so a table cell never counts
    as a rule sentence."""
    return "\n".join(line for line in body.splitlines() if not line.startswith("|"))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--semantics", help="path to a copy of spec/04-semantics.md")
    parser.add_argument("--stdlib", help="path to a copy of spec/05-stdlib.md")
    args = parser.parse_args()

    problems: list[str] = []
    try:
        sem_text = spec_path("04-semantics.md", args.semantics).read_text(encoding="utf-8")
        std_text = spec_path("05-stdlib.md", args.stdlib).read_text(encoding="utf-8")
        workers = section(sem_text, "## Workers and Tasks")
        rows = table(section(workers, "### Worker and Task Faults"), "Kind")
        stdlib_task = section(std_text, "## `task` — workers, tasks, and messages")
    except SpecError as exc:
        report("check_worker_error_kinds.py", [f"cannot read the spec: {exc}"], "")
        return

    sentences = prose(workers) + "\n" + prose(stdlib_task)
    catchable = set(RAISES.findall(sentences))
    ending = set(ENDS_WITH.findall(sentences))
    for kind in sorted(catchable & ending):
        problems.append(f"{kind!r} is both raised and named as ending the program")

    table_kinds: dict[str, str] = {}
    for row in rows:
        try:
            kind = code(row["Kind"])
        except SpecError as exc:
            problems.append(str(exc))
            continue
        if kind in table_kinds:
            problems.append(f"kind {kind!r} has more than one row")
        table_kinds[kind] = row["Catchable"]
        if not row["Message"].startswith("`"):
            problems.append(f"kind {kind!r} has no message text in a code span")
        if not row["Raised when"]:
            problems.append(f"kind {kind!r} has an empty 'Raised when' cell")

    named = catchable | ending
    for kind in sorted(named - table_kinds.keys()):
        problems.append(f"the rule sentences name {kind!r}, but the table has no row for it")
    for kind in sorted(table_kinds.keys() - named):
        problems.append(f"the table has a row for {kind!r}, but no rule sentence raises it")

    for kind, column in table_kinds.items():
        if kind in catchable and column != "yes":
            problems.append(f"{kind!r}: a rule sentence raises it, so Catchable must be 'yes', not {column!r}")
        if kind in ending and column != "no":
            problems.append(f"{kind!r}: it ends the program, so Catchable must be 'no', not {column!r}")

    report(
        "check_worker_error_kinds.py",
        problems,
        f"{len(table_kinds)} kinds ({len(catchable)} catchable, {len(ending)} ending the program); "
        "the table matches the rule sentences",
    )


if __name__ == "__main__":
    main()
