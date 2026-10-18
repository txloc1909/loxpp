#!/usr/bin/env python3
"""
Differential check: a deferred call whose callee is a class or an enum
constructor is a fatal fault on the bootstrap interpreter too. The spec
(spec/04-semantics.md, "Fatal Runtime Errors") allows only a Closure, Native,
BoundMethod or BoundNative as a deferred callee. Any other value halts the
program with "Deferred callable has unexpected type." when the deferred call
is due; the call does not run. Because the fault is fatal, a live `try` does
not catch it.

A run that exits 0, or that runs the deferred class or enum constructor, is
the defect. Each case also pins the expected stdout and exit status as data,
so a bug that native and the bootstrap share cannot pass. Native appends a
stack trace to stderr that the bootstrap does not, so stderr is compared on
its first line only.

A release build is required (the same --trace-enabled skip rule as the other
bootstrap ctest entries): a debug build's instruction trace floods native's
stdout.

Usage:
    tools/check_bootstrap_defer_callable.py [--native PATH] [--bootstrap PATH]
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

SKIP_RETURN_CODE = 125

UNEXPECTED_TYPE = "Deferred callable has unexpected type."


@dataclass
class Case:
    name: str
    program: str
    stdout: list[str]
    exit_code: int
    stderr_first: str = ""


CASES = [
    Case(
        "issue 671 example: a deferred class",
        'class C {}\nfun g() { defer C(); print "body"; }\ng();\n',
        ["body"], 70, UNEXPECTED_TYPE,
    ),
    Case(
        "issue 671 example: a deferred enum constructor",
        'enum E { A(x) }\nfun g() { defer A(1); print "body"; }\ng();\n',
        ["body"], 70, UNEXPECTED_TYPE,
    ),
    Case(
        "a deferred class with an init does not run the init",
        'class C { init(x) { print "init"; } }\n'
        'fun g() { defer C(1); print "body"; }\ng();\n',
        ["body"], 70, UNEXPECTED_TYPE,
    ),
    Case(
        "a deferred class drained by an uncaught throw",
        'class C {}\nfun g() { defer C(); print "body"; throw "x"; }\ng();\n',
        ["body"], 70, UNEXPECTED_TYPE,
    ),
    Case(
        "a deferred enum constructor drained by an uncaught throw",
        'enum E { A(x) }\n'
        'fun g() { defer A(1); print "body"; throw "x"; }\ng();\n',
        ["body"], 70, UNEXPECTED_TYPE,
    ),
    Case(
        "a live try does not catch the fault of a deferred class",
        'class C {}\nfun g() { defer C(); print "body"; }\n'
        'try { g(); } catch (e) { print "caught"; }\nprint "after";\n',
        ["body"], 70, UNEXPECTED_TYPE,
    ),
    Case(
        "the fault skips the earlier defers",
        'class C {}\nfun note(s) { print s; }\n'
        'fun g() { defer note("d1"); defer C(); print "body"; }\ng();\n',
        ["body"], 70, UNEXPECTED_TYPE,
    ),
    Case(
        "control: a deferred function runs",
        'fun note(s) { print s; }\n'
        'fun g() { defer note("d1"); print "body"; }\ng();\nprint "after";\n',
        ["body", "d1", "after"], 0,
    ),
    Case(
        "control: a deferred bound method runs",
        'class C { m() { print "m"; } }\n'
        'fun g() { var c = C(); defer c.m(); print "body"; }\ng();\nprint "after";\n',
        ["body", "m", "after"], 0,
    ),
    Case(
        "control: a deferred native function is accepted",
        'fun g() { defer clock(); print "body"; }\ng();\nprint "after";\n',
        ["body", "after"], 0,
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

    print(f"OK: {len(CASES)} deferred-callable case(s) match between native and bootstrap.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
