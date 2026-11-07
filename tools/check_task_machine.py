#!/usr/bin/env python3
"""Checks the tables of "The Task Machine" in spec/04-semantics.md.

The rules:

  - Each task state of the move table is a row of the task state table, and
    each move has a unique name.
  - Each task state is the `To` state of at least one move. Each task state
    but `ended` is the `From` state of at least one move, and `ended` is the
    `From` state of none: an ended task never runs again.
  - Each task state but `ended` has a move to a state that is not `ended`, so
    a task in it can continue, and each task state can be reached from a new
    task by the moves.
  - The task states, the worker states and the moves are pinned in both
    directions: TASK_STATES, WORKER_STATES and MOVES name each row, so a row
    removed from the spec fails, and so does a row that this script does not
    know. A row added to the machine is a change of the machine, and it
    changes this script in the same pull request.
  - The Event cell of each move holds the words that MOVE_EVENTS gives it, so
    a move cannot change its event (for example from class suspends to class
    blocks) without a change here.
  - Each move has the From and To states that MOVES gives it, and a move whose
    To state is `ready` names a place in the run queue (not `—`).
  - The table of task-owned state has a row for each item in REQUIRED_OWNED.
    These are the items that a suspension overwrote in a named bug class
    (docs/decisions/concurrency-semantics-grounding.md, "Constraints from
    named bug classes"), so a row removed from the spec fails here.
  - The calls of the sides table are exactly the calls of class `suspends`
    on a Socket or a Server in the Blocking calls table of spec/05-stdlib.md,
    and each call is on one side only.

A row removed from either side of a rule fails.

Usage:
    tools/check_task_machine.py [--semantics PATH] [--stdlib PATH]
"""

import argparse
import re

from spec_tables import SpecError, code, report, section, spec_path, table

NEW_TASK = "—"
ENDED = "ended"
TASK_STATES = ("ready", "running", "receiving", "waiting", "ended")
WORKER_STATES = ("active", "blocked", "idle", "finished")
# move -> (From states, To state)
MOVES = {
    "spawn": ({NEW_TASK}, "ready"),
    "spawn-worker": ({NEW_TASK}, "ready"),
    "run": ({"ready"}, "running"),
    "yield": ({"running"}, "ready"),
    "receive": ({"running"}, "receiving"),
    "message": ({"receiving"}, "ready"),
    "limit": ({"receiving"}, "ready"),
    "wait": ({"running"}, "waiting"),
    "wait-end": ({"waiting"}, "ready"),
    "end": ({"running"}, "ended"),
    "abandon": ({"ready", "receiving", "waiting"}, "ended"),
}
# move -> words that its Event cell must hold
MOVE_EVENTS = {
    "spawn": ("`task.spawn(fn)`",),
    "spawn-worker": ("`task.spawn_worker(fn, msg)`",),
    "run": ("run queue is not empty",),
    "yield": ("`yield`", "outside every coroutine"),
    "receive": ("receive", "can suspend"),
    "message": ("message arrives",),
    "limit": ("limit", "receive"),
    "wait": ("class suspends", "can suspend"),
    "wait-end": ("its event happens", "closes", "limit passes"),
    "end": ("returns", "uncaught throw", "halting fault"),
    "abandon": ("the program ends",),
}
REQUIRED_OWNED = (
    "the mailbox",
    "the current task",
    "the call frames",
    "the deferred calls",
    "the handlers",
    "the current coroutine",
    "the contexts",
    "the overflow unwind",
    "the waiting call",
)
SIDE_OBJECTS = ("Socket.", "Server.")


def spans(cell: str) -> list[str]:
    """Returns the code spans of a cell that lists one or more states."""
    found = re.findall(r"`([^`]+)`", cell)
    if not found:
        raise SpecError(f"cell names no state: {cell!r}")
    return found


def pin(kind: str, found: list[str], known: tuple[str, ...], problems: list[str]) -> None:
    """A row that the spec lacks, or that this script does not know, fails."""
    for name in known:
        if name not in found:
            problems.append(f"the spec has no {kind} {name!r}")
    for name in found:
        if name not in known:
            problems.append(f"{kind} {name!r} is not one this script knows; a change of the machine changes this script too")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--semantics", help="path to a copy of spec/04-semantics.md")
    parser.add_argument("--stdlib", help="path to a copy of spec/05-stdlib.md")
    args = parser.parse_args()

    problems: list[str] = []
    try:
        sem_text = spec_path("04-semantics.md", args.semantics).read_text(encoding="utf-8")
        std_text = spec_path("05-stdlib.md", args.stdlib).read_text(encoding="utf-8")
        body = section(section(sem_text, "## Workers and Tasks"), "### The Task Machine")
        task_rows = table(body, "Task state")
        worker_rows = table(body, "Worker state")
        move_rows = table(body, "Move")
        owned_rows = table(body, "Task-owned state")
        side_rows = table(body, "Side")
        blocking_rows = table(section(std_text, "## Blocking calls"), "Call")
    except SpecError as exc:
        report("check_task_machine.py", [f"cannot read the spec: {exc}"], "")
        return

    states: list[str] = []
    for row in task_rows:
        try:
            state = code(row["Task state"])
        except SpecError as exc:
            problems.append(f"task state table: {exc}")
            continue
        if state in states:
            problems.append(f"task state {state!r} appears more than once")
        states.append(state)
    if ENDED not in states:
        problems.append(f"the task state table has no row {ENDED!r}")
    if not worker_rows:
        problems.append("the worker state table has no rows")
    pin("task state", states, TASK_STATES, problems)
    worker_states: list[str] = []
    for row in worker_rows:
        try:
            worker_states.append(code(row["Worker state"]))
        except SpecError as exc:
            problems.append(f"worker state table: {exc}")
    pin("worker state", worker_states, WORKER_STATES, problems)

    moves: list[str] = []
    edges: set[tuple[str, str]] = set()
    sources: set[str] = set()
    targets: set[str] = set()
    for row in move_rows:
        try:
            move = code(row["Move"])
            froms = spans(row["From"])
            tos = spans(row["To"])
        except SpecError as exc:
            problems.append(f"move table: {exc}")
            continue
        if move in moves:
            problems.append(f"move {move!r} appears more than once")
        moves.append(move)
        if len(tos) != 1:
            problems.append(f"move {move!r} must have one To state, found {tos}")
        for state in froms + tos:
            if state != NEW_TASK and state not in states:
                problems.append(f"move {move!r} names {state!r}, which is not a task state")
        if NEW_TASK in tos:
            problems.append(f"move {move!r} has {NEW_TASK!r} as its To state")
        if move in MOVES:
            want_from, want_to = MOVES[move]
            if set(froms) != want_from or tos != [want_to]:
                problems.append(
                    f"move {move!r} goes {sorted(froms)} -> {tos}, the machine says "
                    f"{sorted(want_from)} -> [{want_to!r}]"
                )
        for word in MOVE_EVENTS.get(move, ()):
            if word not in row["Event"]:
                problems.append(f"the event of move {move!r} does not say {word!r}")
        if tos == ["ready"] and row["Run queue"].strip() in ("", "—"):
            problems.append(f"move {move!r} makes a task ready but names no place in the run queue")
        sources.update(s for s in froms if s != NEW_TASK)
        targets.update(tos)
        edges.update((f, t) for f in froms for t in tos)
    pin("move", moves, tuple(MOVES), problems)
    if not any(NEW_TASK in spans(row["From"]) for row in move_rows if "`" in row["From"]):
        problems.append("no move makes a new task")
    for state in states:
        if state not in targets:
            problems.append(f"task state {state!r} is the To state of no move")
        if state == ENDED:
            if state in sources:
                problems.append(f"task state {ENDED!r} is the From state of a move")
        elif state not in sources:
            problems.append(f"task state {state!r} is the From state of no move")

    for state in states:
        if state != ENDED and not any(f == state and t != ENDED for f, t in edges):
            problems.append(f"task state {state!r} has no move to a state other than {ENDED!r}")
    reached = {NEW_TASK}
    while True:
        more = {t for f, t in edges if f in reached} - reached
        if not more:
            break
        reached |= more
    for state in states:
        if state not in reached:
            problems.append(f"task state {state!r} cannot be reached from a new task")

    owned = [row["Task-owned state"] for row in owned_rows]
    for item in REQUIRED_OWNED:
        if item not in owned:
            problems.append(f"the task-owned state table has no row {item!r}")
    for item in owned:
        if owned.count(item) > 1:
            problems.append(f"task-owned state {item!r} appears more than once")
            break

    suspend_calls: set[str] = set()
    for row in blocking_rows:
        try:
            call = code(row["Call"])
        except SpecError as exc:
            problems.append(f"Blocking calls table: {exc}")
            continue
        if row["Class"] == "suspends" and call.startswith(SIDE_OBJECTS):
            suspend_calls.add(call)
    side_of: dict[str, str] = {}
    for row in side_rows:
        side = row["Side"]
        for call in re.findall(r"`([^`]+)`", row["Calls"]):
            if call in side_of:
                problems.append(f"call {call!r} is on side {side_of[call]!r} and on side {side!r}")
            side_of[call] = side
    for call in sorted(suspend_calls - side_of.keys()):
        problems.append(f"call {call!r} is of class suspends on a Socket or a Server, but no side lists it")
    for call in sorted(side_of.keys() - suspend_calls):
        problems.append(f"side {side_of[call]!r} lists {call!r}, which is not a Socket or Server call of class suspends")

    report(
        "check_task_machine.py",
        problems,
        f"{len(states)} task states, {len(worker_rows)} worker states, {len(moves)} moves, "
        f"{len(owned)} task-owned items, {len(side_of)} calls on {len(side_rows)} sides; the tables agree",
    )


if __name__ == "__main__":
    main()
