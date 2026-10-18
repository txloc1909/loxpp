#!/usr/bin/env python3
"""
Differential check: a Lox++ function called with the wrong argument count
takes the fatal fast path on the bootstrap interpreter too. With no `try`
block active in the running task, the fault is fatal and skips every pending
deferred call; with one active, it is the catchable "ArityError" and the
pending deferred calls run (spec/04-semantics.md, "Fatal Runtime Errors" and
"Wrong argument count's fatal fast path").

A run that prints its pending defer, or that exits 0, is the defect. Each
case also pins the expected stdout and exit status as data, so a bug that
native and the bootstrap share cannot pass. Native appends a stack trace to
stderr that the bootstrap does not, so stderr is compared on its first line
only.

A release build is required (the same --trace-enabled skip rule as the other
bootstrap ctest entries): a debug build's instruction trace floods native's
stdout.

Usage:
    tools/check_bootstrap_arity_fatal.py [--native PATH] [--bootstrap PATH]
        [--timeout SECONDS]

Exits 0 when every case matches. Exits 1 and prints every mismatch
otherwise. Exits 125 (ctest's SKIP_RETURN_CODE) when the binary under test
was built with the debug trace macros on.
"""

import argparse
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
PROBES = REPO_ROOT / "test" / "translation-probes"

SKIP_RETURN_CODE = 125

ARITY_1_GOT_0 = "Expected 1 arguments but got 0."


@dataclass
class Case:
    name: str
    program: str
    stdout: list[str]
    exit_code: int
    stderr_first: str = ""


PRELUDE = (
    "fun note(s) { print s; }\n"
    "fun h(a) { return a; }\n"
)

CASES = [
    Case(
        "issue 636 example",
        PRELUDE + 'fun g() { defer note("defer-g"); h(); }\ng();\n',
        [], 70, ARITY_1_GOT_0,
    ),
    Case(
        "probe 57: defer arity fatal fast path",
        (PROBES / "57_defer_arity_fatal_fast_path.lox").read_text(),
        [], 70, ARITY_1_GOT_0,
    ),
    Case(
        "probe 59: return out of try leaks no count",
        (PROBES / "59_return_out_of_try_leak.lox").read_text(),
        [], 70, ARITY_1_GOT_0,
    ),
    Case(
        "class init called with the wrong count",
        "fun note(s) { print s; }\n"
        "class A { init(x) { this.x = x; } }\n"
        'fun g() { defer note("defer-g"); A(); }\ng();\n',
        [], 70, ARITY_1_GOT_0,
    ),
    Case(
        "bound method called with the wrong count",
        "fun note(s) { print s; }\n"
        "class A { m(x) { return x; } }\n"
        'fun g() { defer note("defer-g"); A().m(); }\ng();\n',
        [], 70, ARITY_1_GOT_0,
    ),
    Case(
        "operator overload method with the wrong count",
        "fun note(s) { print s; }\n"
        "class A { __add__(o, p) { return 1; } }\n"
        'fun g() { defer note("defer-g"); var a = A(); print a + a; }\ng();\n',
        [], 70, "Expected 2 arguments but got 1.",
    ),
    Case(
        "deferred call with the wrong count skips the defers of the caller",
        PRELUDE
        + "fun g() { defer h(); }\n"
        'fun k() { defer note("defer-k"); g(); }\nk();\n',
        [], 70, ARITY_1_GOT_0,
    ),
    Case(
        "first resume of a coroutine with the wrong count",
        PRELUDE
        + 'fun g() { defer note("defer-g"); coroutine.create(h).resume(); }\ng();\n',
        [], 70, ARITY_1_GOT_0,
    ),
    Case(
        "a try in the main task does not cover a spawned task",
        PRELUDE
        + 'fun worker() { defer note("defer-w"); h(); }\n'
        "try {\n"
        "    task.spawn(worker);\n"
        "    task.receive();\n"
        '} catch (e) { print "caught " + e.kind; }\n',
        [], 70, ARITY_1_GOT_0,
    ),
    Case(
        "the catch block is not under its own try",
        PRELUDE
        + 'fun f() { defer note("defer-f"); try { throw 1; } catch (e) { h(); } }\nf();\n',
        [], 70, ARITY_1_GOT_0,
    ),
    Case(
        "a caught arity error leaves no try counted behind it",
        PRELUDE
        + 'fun m() { defer note("defer-m"); try { h(); } catch (e) { print e.kind; } h(); }\n'
        "m();\n",
        ["ArityError"], 70, ARITY_1_GOT_0,
    ),
    Case(
        "control: a live try catches the arity error and the defer runs first",
        PRELUDE
        + 'fun g() { defer note("defer-g"); h(); }\n'
        'try { g(); } catch (e) { print "caught " + e.kind; }\n'
        'print "after";\n',
        ["defer-g", "caught ArityError", "after"], 0,
    ),
    Case(
        "control: a live try catches the arity error of a deferred call",
        PRELUDE
        + "fun g() { defer h(); }\n"
        'try { g(); } catch (e) { print "caught " + e.kind; }\n'
        'print "after";\n',
        ["caught ArityError", "after"], 0,
    ),
    Case(
        "control: a live try catches the arity error of a coroutine start",
        PRELUDE
        + "try { coroutine.create(h).resume(); }\n"
        'catch (e) { print "caught " + e.kind; }\n'
        'print "after";\n',
        ["caught ArityError", "after"], 0,
    ),
    Case(
        "control: a class with no init stays catchable with no try",
        "fun note(s) { print s; }\n"
        "class A { }\n"
        'fun g() { defer note("defer-g"); A(1); }\ng();\n',
        ["defer-g"], 70, "Expected 0 arguments but got some.",
    ),
]


@dataclass
class RunResult:
    exit_code: int
    stdout: list[str]
    stderr: list[str]
    timed_out: bool = False


def run_case(cmd: list[str], case: Case, workdir: Path, timeout: float,
             env: dict | None = None) -> RunResult:
    path = workdir / (case.name.replace(" ", "_").replace(":", "") + ".lox")
    path.write_text(case.program)
    try:
        proc = subprocess.run(
            [*cmd, str(path)],
            capture_output=True,
            text=True,
            timeout=timeout,
            env=env,
            stdin=subprocess.DEVNULL,
        )
    except subprocess.TimeoutExpired:
        return RunResult(-1, [], [], timed_out=True)
    return RunResult(proc.returncode, proc.stdout.splitlines(),
                     proc.stderr.splitlines())


def first_line(lines: list[str]) -> str:
    return lines[0] if lines else ""


def check_case(case: Case, native: RunResult, bootstrap: RunResult) -> list[str]:
    problems = []
    for who, result in (("native", native), ("bootstrap", bootstrap)):
        if result.timed_out:
            problems.append(f"{who} timed out")
            continue
        if result.exit_code != case.exit_code:
            problems.append(f"{who} exit {result.exit_code}, expected {case.exit_code}")
        if result.stdout != case.stdout:
            problems.append(f"{who} stdout {result.stdout!r}, expected {case.stdout!r}")
        if case.stderr_first and first_line(result.stderr) != case.stderr_first:
            problems.append(
                f"{who} first stderr line {first_line(result.stderr)!r}, "
                f"expected {case.stderr_first!r}")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--native", default=str(REPO_ROOT / "build" / "loxpp"))
    parser.add_argument("--bootstrap",
                        default=str(REPO_ROOT / "bootstrap" / "lox_wrapper.sh"))
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument(
        "--trace-enabled",
        action="store_true",
        default=os.environ.get("LOXPP_BOOTSTRAP_TRACE_ENABLED") == "1",
        help="skip (ctest SKIP_RETURN_CODE) instead of running: the native binary "
        "under test was built with LOXPP_DEBUG_TRACE_EXECUTION/LOXPP_DEBUG_PRINT_CODE on",
    )
    args = parser.parse_args()

    if args.trace_enabled:
        print("SKIP: native binary was built with debug trace macros on (release preset required)")
        return SKIP_RETURN_CODE

    bootstrap_env = {**os.environ, "LANGUAGE": "LOXPP"}

    failures: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        workdir = Path(tmp)
        for case in CASES:
            native = run_case([args.native], case, workdir, args.timeout)
            bootstrap = run_case([args.bootstrap], case, workdir,
                                 args.timeout, env=bootstrap_env)
            problems = check_case(case, native, bootstrap)
            if problems:
                failures.append(f"{case.name}:\n  " + "\n  ".join(problems))

    if failures:
        print(f"{len(failures)}/{len(CASES)} case(s) fail:\n")
        print("\n\n".join(failures))
        return 1

    print(f"OK: {len(CASES)} wrong-argument-count case(s) match between native and bootstrap.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
