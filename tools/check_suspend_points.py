#!/usr/bin/env python3
"""Checks the suspend-point table in spec/04-semantics.md.

The rule: the default set of suspend points is `yield`, each receive on a
mailbox with no message to take, and each blocking call, and a send is never
one. So the rows whose `Default set` cell is `yes` must be exactly:

  - one row `yield` of class `yield`;
  - one row of class `receive` for each `task.receive*` function that the
    task section of spec/05-stdlib.md has, and no other; each of them but
    `task.receive()` has a spec probe with `// COVERS: receive-point <name>`
    that calls it in its code (outside comments);
  - one row of class `blocking call` for each row of the Blocking calls table
    in spec/05-stdlib.md, and no other.

No row may name a send or a spawn, and the section must say that a send is
never a suspend point.

The `Fair mode` column is reserved for an opt-in mode that adds points under
a flag. This script never reads it, and it skips a row whose `Default set`
cell is `no`: such a row belongs to that mode only. A fair mode can
therefore add rows without a change to this rule or to this script.

Usage:
    tools/check_suspend_points.py [--semantics PATH] [--stdlib PATH] [--probes DIR]
"""

import argparse
import re

from pathlib import Path

from spec_tables import PROBE_DIR, SpecError, code, probe_covers, report, section, spec_path, table

YIELD = "yield"
RECEIVE = "receive"
BLOCKING = "blocking call"
NOT_A_POINT = ("task.send", "task.spawn")
SEND_SENTENCE = "A send is never a\nsuspend point"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--semantics", help="path to a copy of spec/04-semantics.md")
    parser.add_argument("--stdlib", help="path to a copy of spec/05-stdlib.md")
    parser.add_argument("--probes", default=str(PROBE_DIR), help="the spec probe directory")
    args = parser.parse_args()

    problems: list[str] = []
    try:
        sem_text = spec_path("04-semantics.md", args.semantics).read_text(encoding="utf-8")
        std_text = spec_path("05-stdlib.md", args.stdlib).read_text(encoding="utf-8")
        points_body = section(section(sem_text, "## Workers and Tasks"), "### Suspend Points")
        rows = table(points_body, "Point")
        blocking_rows = table(section(std_text, "## Blocking calls"), "Call")
        task_body = section(std_text, "## `task` — workers, tasks, and messages")
        covers = probe_covers(Path(args.probes))
    except SpecError as exc:
        report("check_suspend_points.py", [f"cannot read the spec: {exc}"], "")
        return

    if " ".join(SEND_SENTENCE.split()) not in " ".join(points_body.split()):
        problems.append("the section does not say 'A send is never a suspend point'")

    blocking_calls = []
    for row in blocking_rows:
        try:
            blocking_calls.append(code(row["Call"]))
        except SpecError as exc:
            problems.append(f"Blocking calls table: {exc}")

    # "### `task.receive_within(seconds) -> Any`" names the point
    # `task.receive_within(seconds)`.
    receive_points = [
        m.group(1)
        for m in re.finditer(r"^### `(task\.receive[a-z_]*\([^)]*\)) -> ", task_body, re.MULTILINE)
    ]

    by_class: dict[str, list[str]] = {YIELD: [], RECEIVE: [], BLOCKING: []}
    for row in rows:
        if any(name in row["Point"] for name in NOT_A_POINT):
            problems.append(f"row {row['Point']!r} names a send or a spawn, which is never a suspend point")
        default = row["Default set"]
        if default == "no":
            continue
        try:
            point = code(row["Point"])
        except SpecError as exc:
            problems.append(str(exc))
            continue
        if default != "yes":
            problems.append(f"row {point!r}: Default set must be 'yes' or 'no', not {default!r}")
            continue
        cls = row["Class"]
        if cls not in by_class:
            problems.append(f"row {point!r}: class {cls!r} is not yield, receive, or blocking call")
            continue
        if point in by_class[cls]:
            problems.append(f"row {point!r} appears more than once")
        by_class[cls].append(point)

    if by_class[YIELD] != ["yield"]:
        problems.append(f"the default set must have exactly one yield row `yield`, found {by_class[YIELD]}")
    if "task.receive()" not in receive_points:
        problems.append("spec/05-stdlib.md has no section for `task.receive()`")
    for point in receive_points:
        if point not in by_class[RECEIVE]:
            problems.append(f"receive {point!r} is in spec/05-stdlib.md but is not a suspend point row")
    for point in by_class[RECEIVE]:
        if point not in receive_points:
            problems.append(f"suspend point {point!r} has no section in the task chapter of spec/05-stdlib.md")
    for call in blocking_calls:
        if call not in by_class[BLOCKING]:
            problems.append(f"blocking call {call!r} is in spec/05-stdlib.md but is not a suspend point row")
    for point in by_class[BLOCKING]:
        if point not in blocking_calls:
            problems.append(f"suspend point {point!r} is not in the Blocking calls table of spec/05-stdlib.md")

    for point in receive_points:
        if point == "task.receive()":
            continue
        name = point.split("(")[0]
        if ("receive-point", name) not in covers:
            problems.append(f"receive {point!r} has no probe with '// COVERS: receive-point {name}'")
    for words, paths in covers.items():
        if words[0] != "receive-point":
            continue
        if len(words) != 2:
            problems.append(f"COVERS {' '.join(words)}: a receive point takes one name")
            continue
        for path in paths:
            code_text = "\n".join(
                line for line in path.read_text(encoding="utf-8").splitlines() if not line.lstrip().startswith("//")
            )
            if not re.search(re.escape(words[1]) + r"\(", code_text):
                problems.append(f"{path.name} has 'COVERS: receive-point {words[1]}' but its code never calls it")

    report(
        "check_suspend_points.py",
        problems,
        f"default set: 1 yield, {len(by_class[RECEIVE])} receives, {len(by_class[BLOCKING])} blocking calls; "
        "no send; the table matches the rule",
    )


if __name__ == "__main__":
    main()
