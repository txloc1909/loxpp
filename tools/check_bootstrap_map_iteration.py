#!/usr/bin/env python3
"""
Differential check: a net-zero Map change during the bootstrap's for-in
(issue #720).

The bootstrap compares the Map's structural version with a snapshot taken
before the loop, not its size, so an erase plus an insert that restores the
net size still faults.

CASES below cover both net-zero orders, a net-zero change on the last
iteration, and two permitted operations that are not structural changes.
Every case runs on native and on
the bootstrap. A halting case must be non-zero on both, and its stdout must
match; the exact status and the message are implementation-defined
(spec/README.md, Conformance). A permitted case must be zero on both.

Release build only: a debug build's LOXPP_DEBUG_TRACE_EXECUTION/LOXPP_DEBUG_PRINT_CODE floods
native's stdout with a disassembly of bootstrap/loxpp_interpreter.lox
itself, so this check skips itself (ctest's SKIP_RETURN_CODE) rather than
compare against noise.

Usage:
    tools/check_bootstrap_map_iteration.py [--native PATH] [--bootstrap PATH]
                                            [--timeout SECONDS]

Exits 0 when every case's bootstrap run matches native's stdout and its
zero/non-zero outcome. Exits 1 and prints every mismatch otherwise. Exits
125 (ctest's SKIP_RETURN_CODE) when --trace-enabled says the binary under
test was built with the debug trace macros on.
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


@dataclass
class Case:
    name: str
    program: str  # full source text, including trailing newline
    halts: bool


CASES = [
    Case(
        "del_then_insert_net_zero",
        """\
var m = {1: "a", 2: "b"};
for (var k in m) {
    print "body";
    m.del(k);
    m[3] = "c";
}
print "after";
""",
        halts=True,
    ),
    Case(
        "insert_then_del_net_zero",
        """\
var m = {1: "a"};
for (var k in m) {
    print "body";
    m[2] = k;
    m.del(2);
}
print "after";
""",
        halts=True,
    ),
    # A mutation on the last iteration has no later binding to catch it.
    # The pre-body check cannot see it, so this case pins the post-body
    # check on its own.
    Case(
        "net_zero_on_last_iteration",
        """\
var m = {1: "a", 2: "b"};
var seen = 0;
for (var k in m) {
    seen = seen + 1;
    print "body";
    if (seen == 2) {
        m.del(k);
        m[3] = "c";
    }
}
print "after";
""",
        halts=True,
    ),
    Case(
        "overwrite_existing_key_is_allowed",
        """\
var m = {1: "a"};
for (var k in m) {
    m[k] = "b";
}
print "after";
""",
        halts=False,
    ),
    Case(
        "delete_missing_key_is_allowed",
        """\
var m = {1: "a"};
for (var k in m) {
    m.del(k + 100);
}
print "after";
""",
        halts=False,
    ),
]


@dataclass
class RunResult:
    exit_code: int
    stdout: str
    stderr: str
    timed_out: bool = False


def run_case(cmd: list[str], case: Case, workdir: Path, timeout: float, env: dict | None = None) -> RunResult:
    program_path = workdir / f"{case.name}.lox"
    program_path.write_text(case.program)
    try:
        proc = subprocess.run(
            [*cmd, str(program_path)],
            capture_output=True,
            text=True,
            timeout=timeout,
            env=env,
        )
    except subprocess.TimeoutExpired:
        return RunResult(exit_code=-1, stdout="", stderr="", timed_out=True)
    return RunResult(exit_code=proc.returncode, stdout=proc.stdout.strip(), stderr=proc.stderr.strip())


def check_case(case: Case, native: RunResult, bootstrap: RunResult) -> list[str]:
    problems = []
    if native.timed_out:
        problems.append("native timed out -- corpus bug, fix the case")
        return problems
    if bootstrap.timed_out:
        problems.append("bootstrap timed out")
        return problems
    # A halt needs only a non-zero status, and a permitted run needs zero:
    # the exact status and every message are implementation-defined
    # (spec/README.md, Conformance).
    native_halts = native.exit_code != 0
    bootstrap_halts = bootstrap.exit_code != 0
    if native_halts != case.halts:
        problems.append(f"native exit {native.exit_code} does not match the case's halts={case.halts}")
    if bootstrap_halts != case.halts:
        problems.append(f"bootstrap exit {bootstrap.exit_code} does not match the case's halts={case.halts}")
    if bootstrap.stdout != native.stdout:
        problems.append(f"stdout: native={native.stdout!r} bootstrap={bootstrap.stdout!r}")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--native", default=str(REPO_ROOT / "build" / "loxpp"))
    parser.add_argument("--bootstrap", default=str(REPO_ROOT / "bootstrap" / "lox_wrapper.sh"))
    parser.add_argument("--timeout", type=float, default=30.0)
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
            native_result = run_case([args.native], case, workdir, args.timeout)
            bootstrap_result = run_case([args.bootstrap], case, workdir, args.timeout, env=bootstrap_env)
            problems = check_case(case, native_result, bootstrap_result)
            if problems:
                failures.append(f"{case.name}:\n  " + "\n  ".join(problems))

    if failures:
        print(f"{len(failures)}/{len(CASES)} case(s) diverge:\n")
        print("\n\n".join(failures))
        return 1

    print(f"OK: {len(CASES)} Map-iteration case(s) match between native and bootstrap.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
