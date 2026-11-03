#!/usr/bin/env python3
"""Checks that the bootstrap interpreter wraps every `suspends` call.

A call of class `suspends` (spec/05-stdlib.md, "Blocking calls") suspends its
host task where the interpreted task can suspend. The host then runs the other
interpreted tasks on its one thread, and each of them sets
`Interpreter.currentTask`, the record that holds all of a task's state, to its
own task. Where the task cannot suspend (spec/04-semantics.md, "Contexts where
a task cannot suspend"), the call must block the worker instead. So
bootstrap/loxpp_interpreter.lox runs every such call through
Interpreter.callSuspending, which does both: it blocks the worker through
BlockedCall, or it sets `currentTask` back to the waiting task when the host
call returns or raises. The set of calls that take that path is the list in
`isSuspendingHostCall`.

This script holds that list to the spec table, in both directions:

- every `suspends` row is on the list;
- every name on the list is a `suspends` row, so an entry cannot outlive the
  table row it was written for;
- no `blocks` row is on the list: a blocking call runs no other task, so it
  needs no switch back.

It also checks the other half: each class that can dispatch a call of class
`suspends` must hold its negation guard in its own `call`, as written, so a
guard that is deleted or that loses its `!` cannot leave a call unwrapped while
the list still names it. Those classes are `LoxNative` (for `sleep`, `connect`
and `connect_within`), `InterpSocketMethod` and `InterpServerMethod`; a class
that cannot dispatch such a call, `InterpFileMethod` and `InterpProcessMethod`
among them, needs no guard. This half is static, so it covers `connect`
without a program that makes a handshake wait.

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

CALL = re.compile(r"^(?:(\w+)\.)?(\w+)\(")
LIST_FN = re.compile(r"^fun isSuspendingHostCall\(name\) \{\n(.*?)^\}", re.S | re.M)
LISTED_NAME = re.compile(r'name == "([^"]+)"')
# The three classes that can dispatch a `suspends` call, each with the one guard
# line its own `call` must hold. The line is checked as written, not as a
# substring: a guard whose `!` is dropped wraps every call, so `connect` would
# lose the state while this script still saw the name.
DISPATCH_GUARDS = {
    "LoxNative":
        "if (!isSuspendingHostCall(this.name)) return this.dispatch(interp, args);",
    "InterpSocketMethod":
        'if (!isSuspendingHostCall("Socket." + this.name)) {',
    "InterpServerMethod":
        'if (!isSuspendingHostCall("Server." + this.name)) {',
}
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
    """Returns the dispatch classes whose `call` lacks its guard line."""
    problems = []
    for cls, guard in DISPATCH_GUARDS.items():
        start = text.find(f"class {cls} {{")
        if start < 0:
            problems.append(f"the interpreter has no class {cls}")
            continue
        method = CALL_METHOD.search(text, start)
        if method is None:
            problems.append(f"class {cls} has no call(interp, args) method")
            continue
        if guard not in method.group(1):
            problems.append(
                f"class {cls}'s call does not hold the guard {guard!r}, so a call "
                "it dispatches can park without the switch back to its task"
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
            if name not in wrapped:
                problems.append(
                    f"{name!r} is a `suspends` call, but isSuspendingHostCall does not "
                    "name it, so a task that waits in it runs on as another task, or "
                    "suspends where it must block the worker"
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

    problems.extend(unwrapped_dispatch_sites(interpreter_text))

    report(
        "check_bootstrap_suspend_calls.py",
        problems,
        f"{len(classes)} blocking calls; every `suspends` call is wrapped "
        f"({len(wrapped)} wrapped), and all "
        f"{len(DISPATCH_GUARDS)} dispatch sites hold their guard",
    )


if __name__ == "__main__":
    main()
