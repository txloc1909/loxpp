#!/usr/bin/env python3
"""Holds the bootstrap's per-task state to the parks it must survive (issues
#984, #1008, #1009 and #1064).

Each interpreted task is one host task, and all of them run one Interpreter
of bootstrap/loxpp_interpreter.lox. A host task that suspends (a call of class
`suspends`, a receive, a `yield` of the task) lets the other host tasks run,
and each of them runs as its own task. So all the state that belongs to a task
(spec/04-semantics.md, The Task Machine, the task-owned state table) is in
the task's record, InterpTask, and the interpreter reaches it only through
`Interpreter.currentTask`. A park copies nothing: the host task that runs again
sets `currentTask` back to the task it holds, through Interpreter.resumeTask.
Where the task cannot suspend (a deferred call, a `__str__`), a call of class
`suspends` blocks the worker instead (spec/04-semantics.md, "Contexts where a
task cannot suspend"), so no park happens.

This check has these parts.

1. One case per field of the task record, one per call of resumeTask after a
   park, one per dispatch guard of a call of class `suspends`, and the cases
   that pin the block itself. Each case runs on native and on the bootstrap,
   and the bootstrap's stdout and its zero/non-zero outcome must match
   native's. The exact status and every message are implementation-defined
   (spec/README.md, Conformance), so a halting case needs only a non-zero
   status on both.

2. A mutation per case, and the case must then differ from native.
   - `outside_<field>` moves one field of the record to the Interpreter, as
     one value that every task shares: each `currentTask.<field>` becomes
     `<field>` on the Interpreter. So the case shows that the field leaks
     between two tasks when it is not in the record.
   - `outside_globals` does the same for the globals of the worker of the
     task, which the record reaches through its worker.
   - `resume_<site>` removes one call of resumeTask after a park, so the task
     runs on as the task that ran last.
   - `<class>_guard` makes one dispatch site skip Interpreter.callSuspending.

3. The fields `nativeDepth`, `stringifyDepth`, `overflowUnwinding` and
   `overflowDeferNest` are non-zero only in a frame that native reaches from
   C++, where the bootstrap blocks and never parks. A case for one of them
   is a `parking` case: it runs with the mutation `force_park`, which makes
   the bootstrap park there, as a control that must match native, and
   then with the field also moved out of the record, which must differ. A
   program of such a case waits for a line that a process outside the
   program sends, so native can run it to the end.
   The `stringifyDepth` case prints a list nested to the bootstrap's own depth
   limit (an implementation-defined limit that native's is above), so one more
   level from another task makes it a halting fault.

4. Two cases pin the block itself: with `force_park`, a task of the same
   worker runs during the wait, and the output changes.

5. Three rules that keep 2 complete. Every field of the record must have an
   `outside_<field>` mutation. Every call of resumeTask but the one that
   starts a task (Interpreter.runTask) must have a `resume_<site>` mutation.
   No mutation may sit in MUTATIONS that no case runs. So a field or a park
   site that no case covers, or a mutation that quietly stopped running,
   fails rather than passing with a count that includes it.

The three dispatch sites that consult `isSuspendingHostCall` are also pinned by
tools/check_bootstrap_suspend_calls.py.

Release build only: the debug preset's LOXPP_DEBUG_TRACE_EXECUTION and
LOXPP_DEBUG_PRINT_CODE flood native's stdout, so this check skips itself
(ctest's SKIP_RETURN_CODE) rather than compare against noise.

Usage:
    tools/check_bootstrap_park_state.py [--native PATH] [--interpreter PATH]
                                        [--timeout SECONDS] [--trace-enabled]

Exits 0 when every case matches native and every mutation is caught, 1
otherwise, and 125 (ctest's SKIP_RETURN_CODE) when the build under test traces
execution.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from runner_precondition import refuse_unrunnable, require_runner

REPO_ROOT = Path(__file__).resolve().parent.parent

INTERPRETER = REPO_ROOT / "bootstrap" / "loxpp_interpreter.lox"
WRAPPER = REPO_ROOT / "bootstrap" / "lox_wrapper.sh"

SKIP_RETURN_CODE = 125

# The task record: the fields that InterpTask.init sets after the comment that
# opens the record, up to the loxTag line.
RECORD_START = "        // The interpreter state that belongs to this task"
RECORD_END = '        this.loxTag    = "task";'
RECORD_FIELD = re.compile(r"^        this\.(\w+) = (.+);$", re.M)
# The place in Interpreter.init where an `outside_` mutation defines the field.
INTERPRETER_FIELDS = "        this.currentTask = this.mainTask;\n"
RESUME_CALL = re.compile(r"\b(?:this|interp)\.resumeTask\(t\);")
TASK_START_RESUME = "    runTask(t) {\n        this.resumeTask(t);\n"


class MutationError(Exception):
    pass


def record_fields(text: str) -> dict[str, str]:
    """The fields of the task record, each with its start value."""
    start = text.find(RECORD_START)
    end = text.find(RECORD_END, start)
    if start < 0 or end < 0:
        raise MutationError("the interpreter has no task record in InterpTask.init")
    return dict(RECORD_FIELD.findall(text[start:end]))


def pair(old: str, new: str) -> Callable[[str], str]:
    def apply(text: str) -> str:
        n = text.count(old)
        if n != 1:
            raise MutationError(f"matches {n} place(s) in the interpreter, not one")
        return text.replace(old, new)
    return apply


def outside(path: str, define: str) -> Callable[[str], str]:
    """Moves `currentTask.<path>` to one Interpreter field that every task
    shares, defined in Interpreter.init as `define`."""
    field = path.split(".")[-1]

    def apply(text: str) -> str:
        n = text.count(f"currentTask.{path}")
        if n == 0:
            raise MutationError(f"the interpreter never reads currentTask.{path}")
        if text.count(INTERPRETER_FIELDS) != 1:
            raise MutationError("Interpreter.init has no one place for the field")
        text = text.replace(f"currentTask.{path}", field)
        return text.replace(INTERPRETER_FIELDS,
                            INTERPRETER_FIELDS + f"        this.{field} = {define};\n")
    return apply


# A text edit of the interpreter that changes what one case pins.
MUTATIONS: dict[str, Callable[[str], str]] = {
    "native_guard": pair(
        "        if (!isSuspendingHostCall(this.name)) return this.dispatch(interp, args);\n",
        "        return this.dispatch(interp, args);\n",
    ),
    "socket_guard": pair(
        '        if (!isSuspendingHostCall("Socket." + this.name)) {\n',
        "        if (true) {\n",
    ),
    "server_guard": pair(
        '        if (!isSuspendingHostCall("Server." + this.name)) {\n',
        "        if (true) {\n",
    ),
    # The bootstrap parks where the task cannot suspend.
    "force_park": pair(
        "        return this.currentTask.nativeDepth == 0;\n",
        "        return true;\n",
    ),
    "resume_suspend": pair(
        "            throw e;\n        }\n        this.resumeTask(t);\n        return result;\n",
        "            throw e;\n        }\n        return result;\n",
    ),
    "resume_suspend_fault": pair(
        "            result = site.hostBody(this, args);\n        } catch (e) {\n"
        "            this.resumeTask(t);\n",
        "            result = site.hostBody(this, args);\n        } catch (e) {\n",
    ),
    "resume_yield": pair(
        "        yield nil;\n        this.resumeTask(t);\n",
        "        yield nil;\n",
    ),
    "resume_receive": pair(
        "        task.receive();\n        this.resumeTask(t);\n",
        "        task.receive();\n",
    ),
    "resume_select_test": pair(
        "        interp.resumeTask(t);\n",
        "",
    ),
    "resume_select_timeout": pair(
        "            // switch back.\n            this.resumeTask(t);\n",
        "            // switch back.\n",
    ),
    "outside_globals": outside("worker.globals", "globals"),
}

@dataclass
class Case:
    name: str
    covers: str
    program: str
    mutation: str
    # True when the state of this case is live only if the bootstrap parks
    # where the task cannot suspend. Both the control run and the mutated run
    # then apply the mutation `force_park` first.
    parking: bool = False
    # True when the program ends in a halt (spec/README.md, Conformance), so
    # native must exit non-zero.
    halts: bool = False


SETUP = """\
var srv = listen("127.0.0.1", 0);
var port = srv.port();
var peer = connect("127.0.0.1", port);
var conn = srv.accept();
var me = task.self();
fun note() { print "deferred ran"; }
"""


# A process outside the program ends the wait of a blocked call: it connects
# at once and sends one line after a delay, so no task of the program has to
# run for the wait to end.
FEEDER = """\
var srv = listen("127.0.0.1", 0);
var port = srv.port();
var me = task.self();
var feeder = spawn("bash", ["-c", "exec 3<>/dev/tcp/127.0.0.1/" + str(port) +
                                  "; sleep 0.4; echo line >&3; sleep 0.2"]);
var conn = srv.accept();
fun note() { print "deferred ran"; }
fun checker() { task.send(me, "done"); }
"""

# The shape of most cases: `a` parks in a readline with its state set, `b`
# writes the line, sets its own state, and parks in a sleep, so `a` runs again
# while `b` is parked and is the task that ran last.
OVERFLOW = """\
fun deep(n) { return deep(n + 1) + 1; }
fun waitLine() {
    print "defer waits";
    print "defer got " + conn.readline();
    try {
        throw "inner";
    } catch (e) {
        print "inner caught";
    }
    try {
        deep(0);
    } catch (e) {
        print "second caught";
    }
}
fun holder() {
    defer waitLine();
    deep(0);
}
fun first() {
    try {
        holder();
    } catch (e) {
        print "first caught";
    }
    task.send(me, "done");
}
fun second() {
    try {
        deep(0);
    } catch (e) {
    }
    try {
        deep(0);
    } catch (e) {
        task.send(me, "done");
    }
}
task.spawn(first);
task.spawn(second);
task.receive();
task.receive();
"""

CASES = [
    Case(
        "defer_readline",
        "a Socket read that parks",
        SETUP
        + """\
fun worker() {
    defer note();
    print "before";
    print "got " + conn.readline();
}
fun finish() { worker(); task.send(me, "done"); }
fun checker() { print "checker ran"; peer.writeline("line"); task.send(me, "done"); }
task.spawn(finish);
task.spawn(checker);
task.receive();
task.receive();
""",
        "socket_guard",
    ),
    Case(
        "defer_accept",
        "Server.accept that parks",
        SETUP
        + """\
fun acceptor() {
    defer note();
    print "before accept";
    var c = srv.accept();
    print "accepted " + c.readline();
}
fun finish() { acceptor(); task.send(me, "done"); }
fun connector() {
    var p = connect("127.0.0.1", port);
    p.writeline("line");
    task.send(me, "done");
}
task.spawn(finish);
task.spawn(connector);
task.receive();
task.receive();
""",
        "server_guard",
    ),
    Case(
        "defer_sleep",
        "a sleep that parks",
        """\
var me = task.self();
fun note() { print "deferred ran"; }
fun sleeper() {
    defer note();
    print "before";
    sleep(0.2);
    print "after";
}
fun finish() { sleeper(); task.send(me, "done"); }
fun checker() { print "checker ran"; task.send(me, "done"); }
task.spawn(finish);
task.spawn(checker);
task.receive();
task.receive();
""",
        "native_guard",
    ),
    Case(
        "pending_defers_survive",
        "pendingDefers",
        SETUP
        + """\
fun noteB() { print "b deferred"; }
fun a() {
    defer note();
    print "a got " + conn.readline();
}
fun b() {
    defer noteB();
    peer.writeline("line");
    sleep(0.2);
    print "b after sleep";
}
fun finishA() { a(); task.send(me, "done"); }
fun finishB() { b(); task.send(me, "done"); }
task.spawn(finishA);
task.spawn(finishB);
task.receive();
task.receive();
""",
        "outside_pendingDefers",
    ),
    Case(
        "task_handle_survives",
        "currentTask after a call of class suspends",
        SETUP
        + """\
fun a() {
    var self0 = task.self();
    print "a got " + conn.readline();
    print "same task: " + str(self0 == task.self());
    task.send(me, "done");
}
fun b() { peer.writeline("line"); sleep(0.2); task.send(me, "done"); }
task.spawn(a);
task.spawn(b);
task.receive();
task.receive();
""",
        "resume_suspend",
    ),
    Case(
        "socket_fault_survives",
        "currentTask after a Socket call that a close wakes",
        SETUP
        + """\
fun reader() {
    var self0 = task.self();
    defer note();
    try {
        print "before " + conn.readline();
    } catch (e) {
        print "caught " + e.kind;
    }
    print "same task: " + str(self0 == task.self());
    task.send(me, "done");
}
fun closer() { conn.close(); sleep(0.2); task.send(me, "done"); }
task.spawn(reader);
task.spawn(closer);
task.receive();
task.receive();
""",
        "resume_suspend_fault",
    ),
    Case(
        "yield_survives",
        "currentTask after a yield of the task",
        """\
var me = task.self();
fun a() {
    var self0 = task.self();
    yield;
    print "same task: " + str(self0 == task.self());
    task.send(me, "done");
}
fun b() { print "b ran"; task.send(me, "done"); }
task.spawn(a);
task.spawn(b);
task.receive();
task.receive();
""",
        "resume_yield",
    ),
    Case(
        "receive_survives",
        "currentTask after task.receive()",
        """\
var me = task.self();
fun a() {
    var self0 = task.self();
    print "a got " + str(task.receive());
    print "same task: " + str(self0 == task.self());
    task.send(me, "done");
}
var ta = task.spawn(a);
fun b() { task.send(ta, 1); task.send(me, "done"); }
task.spawn(b);
task.receive();
task.receive();
""",
        "resume_receive",
    ),
    Case(
        "selective_receive_survives",
        "currentTask in the test of a receive with a condition",
        """\
var me = task.self();
fun two(m) { return m == 2; }
fun a() {
    var self0 = task.self();
    print "a got " + str(task.receive_where(two));
    print "same task: " + str(self0 == task.self());
    print "left " + str(task.pending());
    task.send(me, "done");
}
var ta = task.spawn(a);
fun b() { task.send(ta, 1); yield; task.send(ta, 2); task.send(me, "done"); }
task.spawn(b);
task.receive();
task.receive();
""",
        "resume_select_test",
    ),
    Case(
        "receive_limit_survives",
        "currentTask after the limit of a receive",
        """\
var me = task.self();
fun a() {
    var self0 = task.self();
    try {
        task.receive_within(0.1);
    } catch (e) {
        print "caught " + e.kind;
    }
    print "same task: " + str(self0 == task.self());
    task.send(me, "done");
}
fun b() { sleep(0.3); task.send(me, "done"); }
task.spawn(a);
task.spawn(b);
task.receive();
task.receive();
""",
        "resume_select_timeout",
    ),
    Case(
        "worker_globals_survive",
        "the globals of the worker",
        # A top-level `fun` is a global variable of each worker. The main
        # worker assigns its own, and the other worker defines its own copy,
        # so each reads its own.
        """\
var me = task.self();
fun who() { return "declared"; }
fun changed() { return "changed"; }
fun entry(main) {
    print "worker sees " + who();
    task.send(main, "done");
}
who = changed;
task.spawn_worker(entry, me);
task.receive();
print "main sees " + who();
""",
        "outside_globals",
    ),
    Case(
        "try_depth_survives",
        "tryDepth",
        # `a` has no try statement, so its overflow is fatal and skips its
        # pending deferred call. `b` parks inside a try block.
        SETUP
        + """\
fun deep(n) { return deep(n + 1) + 1; }
fun a() {
    defer note();
    print "a got " + conn.readline();
    deep(0);
}
fun b() {
    try {
        peer.writeline("line");
        sleep(0.3);
    } catch (e) {
        print "b caught " + e.kind;
    }
}
task.spawn(a);
task.spawn(b);
task.receive();
""",
        "outside_tryDepth",
        halts=True,
    ),
    Case(
        "native_depth_survives",
        "nativeDepth",
        # `worker` parks in a deferred call, the park that force_park makes.
        # `other` receives meanwhile, which only a task outside every native
        # frame can. Native blocks the worker, so `other` runs later there,
        # and its result is printed after both tasks end.
        FEEDER
        + """\
fun inner() { print "inner got " + conn.readline(); }
fun outer() {
    defer inner();
    print "outer returns";
}
fun worker() { outer(); task.send(me, "done"); }
var seen = nil;
fun other() {
    try {
        task.receive_within(0.1);
    } catch (e) {
        seen = e.kind;
    }
    task.send(me, "done");
}
task.spawn(worker);
task.spawn(other);
task.receive();
task.receive();
print "other caught " + seen;
feeder.wait();
""",
        "outside_nativeDepth",
        parking=True,
    ),
    Case(
        "stringify_depth_survives",
        "stringifyDepth",
        # `worker` parks inside the __str__ that print calls, the park that
        # force_park makes, and `other` prints a list of 100 nested lists
        # meanwhile: the depth limit of the bootstrap.
        FEEDER
        + """\
class P {
    init() {}
    __str__() {
        print "str before";
        print "str got " + conn.readline();
        return "P";
    }
}
fun worker() { print P(); task.send(me, "done"); }
var text = nil;
fun other() {
    var deep = [];
    var i = 0;
    while (i < 99) {
        deep = [deep];
        i = i + 1;
    }
    text = str(deep);
    task.send(me, "done");
}
task.spawn(worker);
task.spawn(other);
task.receive();
task.receive();
print text;
feeder.wait();
""",
        "outside_stringifyDepth",
        parking=True,
    ),
    Case(
        "coroutine_survives",
        "currentCo",
        # `a` parks inside the coroutine it resumed, and `b` parks inside its
        # own. Each coroutine then yields to its own resumer.
        SETUP
        + """\
fun body() {
    print "co got " + conn.readline();
    yield "yielded";
}
var co = coroutine.create(body);
fun body2() {
    peer.writeline("line");
    sleep(0.2);
    yield "y2";
}
var co2 = coroutine.create(body2);
fun a() { print "a resume: " + str(co.resume()); task.send(me, "done"); }
fun b() { print "b resume: " + str(co2.resume()); task.send(me, "done"); }
task.spawn(a);
task.spawn(b);
task.receive();
task.receive();
""",
        "outside_currentCo",
    ),
    # The unwind of a stack overflow belongs to its task. `first` parks in
    # its deferred call while it unwinds (the park that force_park makes),
    # `second` runs and overflows twice meanwhile, each time caught, and
    # `first` then overflows a second time during its own unwind, which is
    # fatal. If `second` sees the unwind of `first`, one of its overflows is
    # fatal, and the program halts before `first` gets its line.
    Case(
        "overflow_unwinding_survives",
        "overflowUnwinding",
        FEEDER + OVERFLOW,
        "outside_overflowUnwinding",
        parking=True,
        halts=True,
    ),
    Case(
        "overflow_defer_nest_survives",
        "overflowDeferNest",
        FEEDER + OVERFLOW,
        "outside_overflowDeferNest",
        parking=True,
        halts=True,
    ),
    # Where the task cannot suspend, a suspends call blocks the worker: the
    # other task of the same worker waits until the call returns.
    Case(
        "defer_blocks_worker",
        "a suspends call in a deferred call",
        FEEDER
        + """\
fun inner() { print "inner got " + conn.readline(); }
fun outer() { defer inner(); print "outer returns"; }
fun worker() { outer(); print "worker done"; task.send(me, "done"); }
fun noisy() { print "checker ran"; task.send(me, "done"); }
task.spawn(worker);
task.spawn(noisy);
task.receive();
task.receive();
feeder.wait();
""",
        "force_park",
    ),
    Case(
        "str_blocks_worker",
        "a suspends call in the __str__ that print calls",
        FEEDER
        + """\
class P {
    init() {}
    __str__() { print "str got " + conn.readline(); return "P"; }
}
fun worker() { print P(); print "worker done"; task.send(me, "done"); }
fun noisy() { print "checker ran"; task.send(me, "done"); }
task.spawn(worker);
task.spawn(noisy);
task.receive();
task.receive();
feeder.wait();
""",
        "force_park",
    ),
]


@dataclass
class RunResult:
    exit_code: int
    stdout: str
    timed_out: bool = False


def run(runner: list[str], program: str, workdir: Path, timeout: float,
        env: dict | None = None) -> RunResult:
    path = workdir / "case.lox"
    path.write_text(program)
    try:
        proc = subprocess.run(
            [*runner, str(path)],
            capture_output=True,
            text=True,
            timeout=timeout,
            env=env,
        )
    except subprocess.TimeoutExpired:
        return RunResult(exit_code=-1, stdout="", timed_out=True)
    except OSError as exc:
        refuse_unrunnable(runner[0], exc)
    return RunResult(exit_code=proc.returncode, stdout=proc.stdout.strip())


def differs(native: RunResult, other: RunResult) -> bool:
    """True when `other` is not what native showed: a different stdout, or a
    zero/non-zero outcome that disagrees."""
    if other.timed_out:
        return True
    if (other.exit_code != 0) != (native.exit_code != 0):
        return True
    return other.stdout != native.stdout


def make_runner(interpreter_text: str, native: str, directory: Path) -> list[str]:
    """Writes a wrapper and an interpreter into `directory`, and returns the
    command that runs it."""
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "loxpp_interpreter.lox").write_text(interpreter_text)
    wrapper = WRAPPER.read_text()
    wrapper = re.sub(r"^SCRIPT_DIR=.*$", f'SCRIPT_DIR="{directory}"', wrapper,
                     count=1, flags=re.M)
    wrapper = re.sub(r"^LOXPP=.*$", f'LOXPP="{native}"', wrapper, count=1, flags=re.M)
    path = directory / "lox_wrapper.sh"
    path.write_text(wrapper)
    path.chmod(0o755)
    return [str(path)]


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--native", default=str(REPO_ROOT / "build" / "loxpp"))
    parser.add_argument("--interpreter", default=str(INTERPRETER))
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument(
        "--trace-enabled",
        action="store_true",
        default=os.environ.get("LOXPP_BOOTSTRAP_TRACE_ENABLED") == "1",
        help="skip (ctest SKIP_RETURN_CODE) instead of running: the native binary "
        "under test was built with LOXPP_DEBUG_TRACE_EXECUTION/LOXPP_DEBUG_PRINT_CODE on",
    )
    args = parser.parse_args()

    require_runner(args.native)

    if args.trace_enabled:
        print("SKIP: native binary was built with debug trace macros on (release preset required)")
        return SKIP_RETURN_CODE

    env = {**os.environ, "LANGUAGE": "LOXPP"}
    failures: list[str] = []
    caught: set[str] = set()

    interpreter_text = Path(args.interpreter).read_text()
    mutations = dict(MUTATIONS)
    try:
        fields = record_fields(interpreter_text)
    except MutationError as exc:
        failures.append(str(exc))
        fields = {}
    for field, start in fields.items():
        mutations[f"outside_{field}"] = outside(field, start)

    used = {case.mutation for case in CASES}
    for field in fields:
        if f"outside_{field}" not in used:
            failures.append(
                f"the task record holds `{field}`, but no case runs 'outside_{field}', "
                "so nothing shows that the field must stay in the record"
            )
    resume_sites = len(RESUME_CALL.findall(interpreter_text))
    if interpreter_text.count(TASK_START_RESUME) != 1:
        failures.append("Interpreter.runTask does not start with its resumeTask call")
    resume_mutations = [m for m in mutations if m.startswith("resume_")]
    if resume_sites - 1 != len(resume_mutations):
        failures.append(
            f"the interpreter calls resumeTask after a park at {resume_sites - 1} "
            f"place(s), but MUTATIONS has {len(resume_mutations)} resume_ mutation(s), "
            "so a place has no case that shows it is needed"
        )
    for mutation in mutations:
        if mutation not in used:
            failures.append(
                f"the mutation {mutation!r} is in MUTATIONS but no case runs it, "
                "so it proves nothing"
            )

    with tempfile.TemporaryDirectory() as tmp:
        workdir = Path(tmp)
        base = make_runner(interpreter_text, args.native, workdir / "base")

        parked = None
        try:
            parked_text = mutations["force_park"](interpreter_text)
            parked = make_runner(parked_text, args.native, workdir / "parked")
        except MutationError as exc:
            failures.append(f"the mutation 'force_park' {exc}")

        for case in CASES:
            native = run([args.native], case.program, workdir, args.timeout)
            if native.timed_out:
                failures.append(f"{case.name}: native timed out -- fix the case")
                continue
            if (native.exit_code != 0) != case.halts:
                failures.append(
                    f"{case.name}: native exits {native.exit_code}, but the case "
                    f"{'halts' if case.halts else 'ends normally'} -- fix the case"
                )
                continue
            oracle = native
            control_text = interpreter_text
            control = base
            if case.parking:
                if parked is None:
                    continue
                control_text = parked_text
                control = parked
            bootstrap = run(control, case.program, workdir, args.timeout, env=env)
            if differs(oracle, bootstrap):
                failures.append(
                    f"{case.name} ({case.covers}): the bootstrap diverges from native"
                    f"{' with force_park' if case.parking else ''}\n"
                    f"  native:    {oracle.stdout!r}\n"
                    f"  bootstrap: {bootstrap.stdout!r}"
                )
                continue

            if case.mutation not in mutations:
                failures.append(f"{case.name}: no mutation {case.mutation!r}")
                continue
            try:
                mutated_text = mutations[case.mutation](control_text)
            except MutationError as exc:
                failures.append(f"{case.name}: the mutation {case.mutation!r} {exc}")
                continue
            mutated = make_runner(mutated_text, args.native, workdir / f"m_{case.name}")
            result = run(mutated, case.program, workdir, args.timeout, env=env)
            if differs(oracle, result):
                caught.add(case.mutation)
            else:
                failures.append(
                    f"{case.name} ({case.covers}): the mutation {case.mutation!r} "
                    "changes nothing this check can see, so the bootstrap may let "
                    "that part of the state leak between tasks without a failure"
                )

    if failures:
        print(f"{len(failures)} failure(s):\n")
        print("\n\n".join(failures))
        return 1

    print(
        f"OK: {len(CASES)} case(s) match their oracle, the {len(fields)} field(s) of "
        f"the task record and the {resume_sites - 1} resume site(s) each have a case, "
        f"and each of the {len(caught)} mutation(s) that ran was caught."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
