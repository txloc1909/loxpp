#!/usr/bin/env python3
"""Checks that the bootstrap interpreter wraps every `suspends` call.

A call of class `suspends` (spec/05-stdlib.md, "Blocking calls") parks its host
task. The host then runs the other interpreted tasks on its one thread, and
every one of them overwrites the interpreter's per-task state as it runs:
`pendingDefers`, `currentTask`, `globals`, `stringifyDepth`, `nativeDepth`,
`tryDepth`, and `currentCo`. Nothing puts that state back when the parked task
resumes, because the native call returns into the caller's own Lox frame, not
into parkTask's caller. So bootstrap/loxpp_interpreter.lox runs every such call
through Interp.saveHostSuspendState/restoreHostSuspendState, and the set of
calls that take that path is the list in `isSuspendingHostCall`.

This script holds that list to the spec table, in both directions:

- every `suspends` row is on the list, or carries a reason in EXEMPTIONS here;
- every name on the list is a `suspends` row, so an entry cannot outlive the
  table row it was written for;
- no `blocks` row is on the list: a blocking call runs no other task, so it
  has no per-task state to save.

It also checks the other half: each of the three classes that dispatch to a
host native (`LoxNative`, `InterpSocketMethod`, `InterpServerMethod`) must
consult the list in its own `call`, so a guard that is deleted or inverted
cannot leave a call unwrapped while the list still names it. This half is
static because `connect` parks only while a handshake is in flight, and
forcing that costs thousands of connections.

A missing entry is the defect that lost a task's deferred call when the task
resumed after a socket park (issue #984).

Usage:
    tools/check_bootstrap_suspend_calls.py [--stdlib PATH] [--interpreter PATH]
"""

import argparse
import re
from pathlib import Path

from spec_tables import REPO_ROOT, SpecError, code, report, section, spec_path, table

DEFAULT_INTERPRETER = REPO_ROOT / "bootstrap" / "loxpp_interpreter.lox"

# A `suspends` call that the bootstrap does not wrap, with the reason it needs
# no saved state. `sleep` blocks this one-thread implementation instead of
# suspending (spec/README.md, Conformance), so no other interpreted task runs
# while it waits.
EXEMPTIONS = {
    "sleep": "blocks this one-thread implementation instead of suspending",
}

CALL = re.compile(r"^(?:(\w+)\.)?(\w+)\(")
LIST_FN = re.compile(r"^fun isSuspendingHostCall\(name\) \{\n(.*?)^\}", re.S | re.M)
LISTED_NAME = re.compile(r'name == "([^"]+)"')
# The three classes that reach a host native, in the order the file defines
# them. Each one's own `call` is where the guard belongs.
DISPATCH_CLASSES = ("LoxNative", "InterpSocketMethod", "InterpServerMethod")
CALL_METHOD = re.compile(r"^    call\(interp, args\) \{\n(.*?)^    \}", re.S | re.M)


def call_name(cell: str) -> str | None:
    """Returns the qualified call a table `Call` cell names, or None."""
    m = CALL.match(code(cell))
    if not m:
        return None
    prefix, name = m.group(1), m.group(2)
    return f"{prefix}.{name}" if prefix else name


def listed_calls(text: str) -> set[str]:
    """Returns the names in the bootstrap's isSuspendingHostCall list."""
    m = LIST_FN.search(text)
    if not m:
        raise SpecError("bootstrap interpreter has no isSuspendingHostCall(name) function")
    return set(LISTED_NAME.findall(m.group(1)))


def unwrapped_dispatch_sites(text: str) -> list[str]:
    """Returns the dispatch classes whose `call` does not consult the list."""
    problems = []
    for cls in DISPATCH_CLASSES:
        start = text.find(f"class {cls} {{")
        if start < 0:
            problems.append(f"the interpreter has no class {cls}")
            continue
        method = CALL_METHOD.search(text, start)
        if method is None:
            problems.append(f"class {cls} has no call(interp, args) method")
            continue
        if "isSuspendingHostCall(" not in method.group(1):
            problems.append(
                f"class {cls}'s call does not consult isSuspendingHostCall, so a "
                "call it dispatches can park without saving the per-task state"
            )
    return problems


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--stdlib", help="path to a copy of spec/05-stdlib.md")
    parser.add_argument("--interpreter", help="path to a copy of the bootstrap interpreter")
    args = parser.parse_args()

    interpreter_path = Path(args.interpreter or DEFAULT_INTERPRETER)
    problems: list[str] = []
    try:
        text = spec_path("05-stdlib.md", args.stdlib).read_text(encoding="utf-8")
        rows = table(section(text, "## Blocking calls"), "Call")
        interpreter_text = interpreter_path.read_text(encoding="utf-8")
        wrapped = listed_calls(interpreter_text)
    except (SpecError, OSError) as exc:
        report("check_bootstrap_suspend_calls.py", [f"cannot read the input: {exc}"], "")
        return

    classes: dict[str, str] = {}
    for row in rows:
        name = call_name(row["Call"])
        if name is None:
            problems.append(f"row {row['Call']!r} does not name a call")
            continue
        if name in classes:
            problems.append(f"call {name!r} has more than one row")
        classes[name] = row["Class"]

    for name, cls in classes.items():
        if cls == "suspends":
            if name in EXEMPTIONS:
                if name in wrapped:
                    problems.append(
                        f"{name!r} is exempt from the wrapper list, but the list names it"
                    )
            elif name not in wrapped:
                problems.append(
                    f"{name!r} is a `suspends` call, but isSuspendingHostCall does not "
                    "name it, so a task that waits in it loses its per-task state"
                )
        elif name in wrapped:
            problems.append(
                f"{name!r} is a {cls!r} call, not a `suspends` call, but "
                "isSuspendingHostCall names it"
            )

    for name in sorted(wrapped):
        if name not in classes:
            problems.append(
                f"isSuspendingHostCall names {name!r}, which has no row in the table"
            )

    for name in EXEMPTIONS:
        if name not in classes:
            problems.append(f"the exemption for {name!r} outlived its table row")

    problems.extend(unwrapped_dispatch_sites(interpreter_text))

    report(
        "check_bootstrap_suspend_calls.py",
        problems,
        f"{len(classes)} blocking calls; every `suspends` call is wrapped "
        f"({len(wrapped)} wrapped, {len(EXEMPTIONS)} exempt), and all "
        f"{len(DISPATCH_CLASSES)} dispatch sites consult the list",
    )


if __name__ == "__main__":
    main()
