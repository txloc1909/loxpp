#!/usr/bin/env python3
"""Holds the bootstrap's per-task state to the parks it must survive (issue
#984).

A host call of class `suspends` (spec/05-stdlib.md, "Blocking calls") parks its
host task, and the host then runs the other interpreted tasks on its one
thread. Every one of them overwrites bootstrap/loxpp_interpreter.lox's
per-task state: `currentTask`, `globals`, `pendingDefers`, `stringifyDepth`,
`nativeDepth`, `tryDepth` and `currentCo`. The call returns into the caller's
frame, not into parkTask's caller, so Interpreter.saveHostSuspendState and
restoreHostSuspendState put that state back from a list the caller holds.

This check has two parts.

1. One case per field of that state, and one for the Socket and one for the
   Server class of suspending call (a `connect` park cannot be made cheaply, so
   tools/check_bootstrap_suspend_calls.py holds that site to its guard alone).
   Each case runs on native and on the bootstrap, and the bootstrap's stdout
   and its zero/non-zero outcome must match native's. The exact status and
   every message are implementation-defined (spec/README.md, Conformance), so a
   halting case needs only a non-zero status on both. Two cases cannot use
   native as the oracle, and say why below.

2. A mutation per case: the case's own line is removed from a copy of the
   interpreter, and the case must then differ from its oracle. A line that no
   case catches fails this check, because the claim that the state is restored
   would then rest on nothing.

`nativeDepth` and `stringifyDepth` are non-zero only inside a frame that
native reaches from C++, such as a deferred call or the `__str__` of `print`.
A suspends call in such a frame blocks the worker instead of suspending
(spec/04-semantics.md, "Contexts where a task cannot suspend"), so native never
returns from those two programs and cannot be the oracle for them. The
bootstrap parks instead, which is a separate defect (issue #1008), so those
two cases use the unmutated bootstrap as the oracle. What they pin is that
removing the restore line changes what the bootstrap does; they do not say
that parking there is right.

The three dispatch sites that consult `isSuspendingHostCall` are pinned by
tools/check_bootstrap_suspend_calls.py, not here.

Release build only: the debug preset's LOXPP_DEBUG_TRACE_EXECUTION and
LOXPP_DEBUG_PRINT_CODE flood native's stdout, so this check skips itself
(ctest's SKIP_RETURN_CODE) rather than compare against noise.

Usage:
    tools/check_bootstrap_park_state.py [--native PATH] [--interpreter PATH]
                                        [--timeout SECONDS] [--trace-enabled]

Exits 0 when every case matches its oracle and every mutation is caught, 1
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


@dataclass
class Case:
    name: str
    covers: str
    program: str
    mutation: str
    # "native": native runs the program to completion, so it is the oracle.
    # "bootstrap": native blocks the worker where this program waits (the
    # contexts rule, spec/04-semantics.md), so the unmutated bootstrap is the
    # oracle: the mutation must change what the bootstrap itself does.
    oracle: str = "native"


# A text edit of the interpreter that removes what one case pins.
MUTATIONS = {
    "socket_guard": (
        '        if (!isSuspendingHostCall("Socket." + this.name)) {\n',
        "        if (true) {\n",
    ),
    "server_guard": (
        '        if (!isSuspendingHostCall("Server." + this.name)) {\n',
        "        if (true) {\n",
    ),
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
        SETUP
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
fun checker() { peer.writeline("line"); task.send(me, "done"); }
task.spawn(worker);
task.spawn(checker);
task.receive();
task.receive();
""",
        "restore_nativeDepth",
        oracle="bootstrap",
    ),
    Case(
        "stringify_depth_survives",
        "stringifyDepth",
        SETUP
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
fun checker() {
    print "checker ran";
    peer.writeline("line");
    task.send(me, "done");
}
task.spawn(worker);
task.spawn(checker);
task.receive();
task.receive();
""",
        "restore_stringifyDepth",
        oracle="bootstrap",
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
    caught: dict[str, str] = {}

    with tempfile.TemporaryDirectory() as tmp:
        workdir = Path(tmp)
        base = make_runner(Path(args.interpreter).read_text(), args.native, workdir / "base")

        for case in CASES:
            if case.oracle == "native":
                oracle = run([args.native], case.program, workdir, args.timeout)
                if oracle.timed_out:
                    failures.append(f"{case.name}: native timed out -- fix the case")
                    continue
                if oracle.exit_code != 0:
                    failures.append(
                        f"{case.name}: native exits {oracle.exit_code} -- fix the case"
                    )
                    continue
                bootstrap = run(base, case.program, workdir, args.timeout, env=env)
                if differs(oracle, bootstrap):
                    failures.append(
                        f"{case.name} ({case.covers}): the bootstrap diverges from native\n"
                        f"  native:    {oracle.stdout!r}\n"
                        f"  bootstrap: {bootstrap.stdout!r}"
                    )
                    continue
            else:
                oracle = run(base, case.program, workdir, args.timeout, env=env)
                if oracle.timed_out:
                    failures.append(
                        f"{case.name}: the bootstrap timed out -- fix the case"
                    )
                    continue
                again = run(base, case.program, workdir, args.timeout, env=env)
                if differs(oracle, again):
                    failures.append(
                        f"{case.name} ({case.covers}): the bootstrap run is not "
                        "repeatable, so it cannot be the oracle"
                    )
                    continue

            old, new = MUTATIONS[case.mutation]
            text = Path(args.interpreter).read_text()
            if text.count(old) != 1:
                failures.append(
                    f"{case.name}: the mutation {case.mutation!r} matches "
                    f"{text.count(old)} place(s) in the interpreter, not one"
                )
                continue
            mutated = make_runner(
                text.replace(old, new), args.native, workdir / f"m_{case.mutation}"
            )
            caught_by = caught.setdefault(case.mutation, case.name)
            result = run(mutated, case.program, workdir, args.timeout, env=env)
            if differs(oracle, result):
                caught[case.mutation] = caught_by
            else:
                failures.append(
                    f"{case.name} ({case.covers}): the mutation {case.mutation!r} "
                    "changes nothing this check can see, so the bootstrap may drop "
                    "that part of the state again without a failure"
                )

    if failures:
        print(f"{len(failures)} failure(s):\n")
        print("\n\n".join(failures))
        return 1

    native_oracle = sum(1 for case in CASES if case.oracle == "native")
    print(
        f"OK: {native_oracle} park-state case(s) match native and "
        f"{len(CASES) - native_oracle} pin what the bootstrap does where native "
        f"blocks; each of the {len(MUTATIONS)} mutation(s) of the saved state is caught."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
