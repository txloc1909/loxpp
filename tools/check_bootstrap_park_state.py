#!/usr/bin/env python3
"""Holds the bootstrap's per-task state to the parks it must survive (issues
#984, #1008 and #1009).

A host call of class `suspends` (spec/05-stdlib.md, "Blocking calls") parks its
host task where the task can suspend, and the host then runs the other
interpreted tasks on its one thread. Every one of them overwrites
bootstrap/loxpp_interpreter.lox's per-task state: `currentTask`, `globals`,
`pendingDefers`, `stringifyDepth`, `nativeDepth`, `tryDepth`, `currentCo`,
`overflowUnwinding` and `overflowDeferNest`. The call returns into the caller's
frame, not into parkTask's caller, so Interpreter.saveHostSuspendState and
restoreHostSuspendState put that state back from a list the caller holds.
Where the task cannot suspend (a deferred call, a `__str__`), the call blocks
the worker instead (spec/04-semantics.md, "Contexts where a task cannot
suspend"), so no park and no restore happens.

This check has these parts.

1. One case per field of that state, one for the Socket class and one for the
   Server class of suspending call, and one for the restore on the fault path
   of each of those two classes, where a close wakes the waiting call with a
   catchable `SocketClosedError`. A `connect` park cannot be made cheaply, so
   tools/check_bootstrap_suspend_calls.py holds that site to its guard alone.
   Each case runs on native and on the bootstrap, and the bootstrap's stdout
   and its zero/non-zero outcome must match native's. The exact status and
   every message are implementation-defined (spec/README.md, Conformance), so a
   halting case needs only a non-zero status on both.

2. A mutation per case: the case's own line is changed in a copy of the
   interpreter (removed, or replaced by a form that skips the guard), and the
   case must then differ from native.

3. The fields `nativeDepth`, `stringifyDepth`, `overflowUnwinding` and
   `overflowDeferNest` are non-zero only in a frame that native reaches from
   C++, where the bootstrap blocks and never parks. A case for one of them
   is a `parking` case: it runs with the mutation `force_park`, which makes
   the bootstrap park there, as a control that must match native, and
   then with the restore line also removed, which must differ. A program of
   such a case waits for a line that a process outside the program sends, so
   native can run it to the end.
   The `stringifyDepth` case shows a depth limit that is implementation-defined,
   so it compares with the control run of the bootstrap, not with native.

4. Two cases pin the block itself: with `force_park`, a task of the same
   worker runs during the wait, and the output changes.

5. Two rules that keep 2 complete. Every field that restoreHostSuspendState
   puts back must have a `restore_<field>` mutation, and no mutation may sit in
   MUTATIONS that no case runs. So a saved field that no case covers, or a
   mutation that quietly stopped running, fails rather than passing with a
   count that includes it.

The `yield` park path (Interpreter.yieldTask, which uses parkTask and
unparkTask) cannot hold a non-zero `nativeDepth`, `stringifyDepth`, or
overflow flag: a yield at depth zero is outside every frame that native
reaches from C++, and an unwind runs no such code at depth zero. So no program
can show a lost value there, and the parkTask lines for these fields follow
the restoreHostSuspendState lines by hand.

The three dispatch sites that consult `isSuspendingHostCall` are pinned by
tools/check_bootstrap_suspend_calls.py, not here.

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

from runner_precondition import refuse_unrunnable, require_runner

REPO_ROOT = Path(__file__).resolve().parent.parent

INTERPRETER = REPO_ROOT / "bootstrap" / "loxpp_interpreter.lox"
WRAPPER = REPO_ROOT / "bootstrap" / "lox_wrapper.sh"

SKIP_RETURN_CODE = 125

RESTORE_FN = re.compile(r"^    restoreHostSuspendState\(saved\) \{\n(.*?)^    \}", re.S | re.M)
RESTORE_LINE = re.compile(r"^        this\.(\w+) += saved\[\d+\];$", re.M)


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
    # "native": native is the oracle. "control": the program shows a state
    # through a limit that the implementation defines (spec/README.md,
    # Conformance, Limits), so native cannot be the oracle, and the control
    # run of the bootstrap is: the mutation must change what the bootstrap
    # itself does.
    oracle: str = "native"


# A text edit of the interpreter that changes what one case pins.
MUTATIONS = {
    "socket_guard": (
        '        if (!isSuspendingHostCall("Socket." + this.name)) {\n',
        "        if (true) {\n",
    ),
    "server_guard": (
        '        if (!isSuspendingHostCall("Server." + this.name)) {\n',
        "        if (true) {\n",
    ),
    # The fault path: a close wakes the waiting call, so its catch must restore
    # the state too. The two sites differ in the host call above the catch.
    # Both classes reach the same catch, in Interpreter.callSuspending.
    "suspend_fault_restore": (
        "            result = site.hostBody(this, args);\n        } catch (e) {\n"
        "            this.restoreHostSuspendState(saved);\n",
        "            result = site.hostBody(this, args);\n        } catch (e) {\n",
    ),
    # The bootstrap parks where the task cannot suspend.
    "force_park": (
        "        return this.nativeDepth == 0;\n",
        "        return true;\n",
    ),
    "reset_overflow": (
        "        this.overflowUnwinding = false;\n"
        "        this.overflowDeferNest = 0;\n"
        "        var fn = t.fn;\n",
        "        var fn = t.fn;\n",
    ),
    "restore_overflowUnwinding": ("        this.overflowUnwinding = saved[7];\n", ""),
    "restore_overflowDeferNest": ("        this.overflowDeferNest = saved[8];\n", ""),
    "restore_pendingDefers": ("        this.pendingDefers  = saved[2];\n", ""),
    "restore_currentTask": ("        this.currentTask    = saved[0];\n", ""),
    "restore_globals": ("        this.globals        = saved[1];\n", ""),
    "restore_tryDepth": ("        this.tryDepth       = saved[5];\n", ""),
    "restore_currentCo": ("        this.currentCo      = saved[6];\n", ""),
    "restore_nativeDepth": ("        this.nativeDepth    = saved[4];\n", ""),
    "restore_stringifyDepth": ("        this.stringifyDepth = saved[3];\n", ""),
}


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
        "pending_defers_survive",
        "pendingDefers",
        SETUP
        + """\
fun worker() {
    defer note();
    print "before";
    print "got " + conn.readline();
}
fun finish() { worker(); task.send(me, "done"); }
fun checker() { peer.writeline("line"); task.send(me, "done"); }
task.spawn(finish);
task.spawn(checker);
task.receive();
task.receive();
""",
        "restore_pendingDefers",
    ),
    Case(
        "task_handle_survives",
        "currentTask",
        SETUP
        + """\
fun worker() {
    var self0 = task.self();
    print "before " + conn.readline();
    print "same task: " + str(self0 == task.self());
    task.send(me, "done");
}
fun checker() { peer.writeline("line"); task.send(me, "done"); }
task.spawn(worker);
task.spawn(checker);
task.receive();
task.receive();
""",
        "restore_currentTask",
    ),
    Case(
        "worker_globals_survive",
        "globals",
        # The spawn_worker task runs while the parked task waits, so only a
        # restored `globals` gives the parked task its own worker's
        # declarations. The two workers' lines must not depend on the order in
        # which the scheduler resumes them (spec/04-semantics.md, Workers and
        # Tasks), so the other worker prints both of its lines before it wakes
        # the parked task: every line it writes is then on stdout first.
        """\
var srv = listen("127.0.0.1", 0);
var port = srv.port();
var me = task.self();
var connX = nil;
var tag = "main worker global";
fun note() { print "deferred ran"; }
fun entry(p) {
    var s = connect("127.0.0.1", p);
    print "x got " + s.readline();
    print "x wrote";
    s.writeline("line");
}
fun worker() {
    var c = connX;
    defer note();
    task.send(me, "parking");
    print "worker before " + c.readline();
    print tag;
    task.send(me, "done");
}
task.spawn_worker(entry, port);
connX = srv.accept();
task.spawn(worker);
task.receive();
connX.writeline("go");
task.receive();
""",
        "restore_globals",
    ),
    Case(
        "socket_fault_survives",
        "a Socket call that a close wakes",
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
fun closer() { conn.close(); task.send(me, "done"); }
task.spawn(reader);
task.spawn(closer);
task.receive();
task.receive();
""",
        "suspend_fault_restore",
    ),
    Case(
        "server_fault_survives",
        "an accept that a close wakes",
        """\
var srv = listen("127.0.0.1", 0);
var me = task.self();
fun note() { print "deferred ran"; }
fun acceptor() {
    var self0 = task.self();
    defer note();
    try {
        srv.accept();
    } catch (e) {
        print "caught " + e.kind;
    }
    print "same task: " + str(self0 == task.self());
    task.send(me, "done");
}
fun closer() { srv.close(); task.send(me, "done"); }
task.spawn(acceptor);
task.spawn(closer);
task.receive();
task.receive();
""",
        "suspend_fault_restore",
    ),
    Case(
        "try_depth_survives",
        "tryDepth",
        SETUP
        + """\
fun one(a) { return a; }
fun worker() {
    try {
        print "before " + conn.readline();
        one();
    } catch (e) {
        print "caught " + e.kind;
    }
    task.send(me, "done");
}
fun checker() { peer.writeline("line"); task.send(me, "done"); }
task.spawn(worker);
task.spawn(checker);
task.receive();
task.receive();
""",
        "restore_tryDepth",
    ),
    Case(
        "native_depth_survives",
        "nativeDepth",
        FEEDER
        + """\
fun inner() {
    print "inner got " + conn.readline();
    try {
        task.receive();
    } catch (e) {
        print "caught " + e.kind;
    }
}
fun outer() {
    defer inner();
    print "outer returns";
}
fun worker() { outer(); task.send(me, "done"); }
task.spawn(worker);
task.spawn(checker);
task.receive();
task.receive();
feeder.wait();
""",
        "restore_nativeDepth",
        parking=True,
    ),
    Case(
        "stringify_depth_survives",
        "stringifyDepth",
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
fun worker() {
    print P();
    var deep = [];
    var i = 0;
    while (i < 100) {
        deep = [deep];
        i = i + 1;
    }
    print deep;
    task.send(me, "done");
}
task.spawn(worker);
task.spawn(checker);
task.receive();
task.receive();
feeder.wait();
""",
        "restore_stringifyDepth",
        parking=True,
        oracle="control",
    ),
    Case(
        "coroutine_survives",
        "currentCo",
        SETUP
        + """\
fun body() {
    defer note();
    print "co before";
    print "co got " + conn.readline();
    yield "yielded";
    print "co after";
}
var co = coroutine.create(body);
fun worker() {
    print "resume 1: " + str(co.resume());
    print "status " + co.status();
    print "resume 2: " + str(co.resume());
    print "status " + co.status();
    task.send(me, "done");
}
fun checker() { peer.writeline("line"); task.send(me, "done"); }
task.spawn(worker);
task.spawn(checker);
task.receive();
task.receive();
""",
        "restore_currentCo",
    ),
    # The unwind of a stack overflow belongs to its task. `first` parks in
    # its deferred call while it unwinds (the park that force_park makes),
    # `second` runs and overflows meanwhile, and `first` then overflows a
    # second time during its own unwind, which is fatal. A task that starts
    # with the flags of another, or a park that does not give them back, makes
    # one of the two overflows the wrong kind.
    Case(
        "overflow_unwinding_survives",
        "overflowUnwinding",
        FEEDER
        + """\
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
        task.send(me, "done");
    }
}
task.spawn(first);
task.spawn(second);
task.receive();
task.receive();
""",
        "restore_overflowUnwinding",
        parking=True,
        halts=True,
    ),
    Case(
        "overflow_defer_nest_survives",
        "overflowDeferNest",
        FEEDER
        + """\
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
        task.send(me, "done");
    }
}
task.spawn(first);
task.spawn(second);
task.receive();
task.receive();
""",
        "restore_overflowDeferNest",
        parking=True,
        halts=True,
    ),
    Case(
        "overflow_reset_at_task_start",
        "the reset of the overflow flags at the start of a task",
        FEEDER
        + """\
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
        task.send(me, "done");
    }
}
task.spawn(first);
task.spawn(second);
task.receive();
task.receive();
""",
        "reset_overflow",
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


def restored_fields(text: str) -> list[str]:
    """The interpreter fields that restoreHostSuspendState puts back."""
    m = RESTORE_FN.search(text)
    if m is None:
        raise ValueError("the interpreter has no restoreHostSuspendState(saved) method")
    return RESTORE_LINE.findall(m.group(1))


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
    try:
        fields = restored_fields(interpreter_text)
    except ValueError as exc:
        failures.append(str(exc))
        fields = []
    used = {case.mutation for case in CASES}
    for field in fields:
        if f"restore_{field}" not in MUTATIONS:
            failures.append(
                f"the interpreter restores `{field}`, but MUTATIONS has no "
                f"'restore_{field}', so nothing checks that its restore line is there"
            )
        elif f"restore_{field}" not in used:
            failures.append(
                f"the mutation 'restore_{field}' is in MUTATIONS but no case runs it"
            )
    for mutation in MUTATIONS:
        if mutation not in used:
            failures.append(
                f"the mutation {mutation!r} is in MUTATIONS but no case runs it, "
                "so it proves nothing"
            )

    with tempfile.TemporaryDirectory() as tmp:
        workdir = Path(tmp)
        base = make_runner(interpreter_text, args.native, workdir / "base")

        parked = None
        if interpreter_text.count(MUTATIONS["force_park"][0]) != 1:
            failures.append("the mutation 'force_park' does not match one place")
        else:
            parked_text = interpreter_text.replace(*MUTATIONS["force_park"])
            parked = make_runner(parked_text, args.native, workdir / "parked")

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
            if case.oracle == "control":
                if bootstrap.timed_out:
                    failures.append(f"{case.name}: the bootstrap timed out -- fix the case")
                    continue
                oracle = bootstrap
            elif differs(oracle, bootstrap):
                failures.append(
                    f"{case.name} ({case.covers}): the bootstrap diverges from native"
                    f"{' with force_park' if case.parking else ''}\n"
                    f"  native:    {oracle.stdout!r}\n"
                    f"  bootstrap: {bootstrap.stdout!r}"
                )
                continue

            old, new = MUTATIONS[case.mutation]
            if control_text.count(old) != 1:
                failures.append(
                    f"{case.name}: the mutation {case.mutation!r} matches "
                    f"{control_text.count(old)} place(s) in the interpreter, not one"
                )
                continue
            mutated = make_runner(
                control_text.replace(old, new), args.native,
                workdir / f"m_{case.name}",
            )
            result = run(mutated, case.program, workdir, args.timeout, env=env)
            if differs(oracle, result):
                caught.add(case.mutation)
            else:
                failures.append(
                    f"{case.name} ({case.covers}): the mutation {case.mutation!r} "
                    "changes nothing this check can see, so the bootstrap may drop "
                    "that part of the state without a failure"
                )

    if failures:
        print(f"{len(failures)} failure(s):\n")
        print("\n\n".join(failures))
        return 1

    print(
        f"OK: {len(CASES)} case(s) match their oracle, {len(fields)} restored "
        f"field(s) each have a case, and each of the {len(caught)} mutation(s) "
        "that ran was caught."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
