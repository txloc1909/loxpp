#!/usr/bin/env python3
"""
Differential check: an uncaught throw of a non-Error value reports its
canonical string form on both native and the bootstrap interpreter — which for
an Instance means dispatching __str__ (or the default "ClassName instance").

The report lands on stderr, and native appends a stack trace the bootstrap
does not, so this compares the exit code and the FIRST stderr line (the
message) only, not the full stderr.

Native's stdout is the ground truth for the first line. A case whose native
run does not exit 70 is a corpus bug and is reported, not skipped.

A release build is required (the same --trace-enabled skip rule as the other
bootstrap ctest entries): a debug build's instruction trace floods native's
stdout and its own stderr carries nothing to read.

Usage:
    tools/check_bootstrap_throw_report.py [--native PATH]
        [--bootstrap PATH] [--timeout SECONDS]

Exits 0 when every case's first stderr line and exit code match between
native and bootstrap. Exits 1 and prints every mismatch otherwise. Exits 125
(ctest's SKIP_RETURN_CODE) when the binary under test was built with the debug
trace macros on.
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


# One uncaught throw per shape: an Instance with __str__, an Instance without,
# and two scalars to prove the report still covers the default/plain forms.
CASES = [
    Case(
        "instance_str",
        "class V { __str__() { return \"CUSTOM\"; } }\n"
        "throw V();\n",
    ),
    Case(
        "instance_default",
        "class V {}\n"
        "throw V();\n",
    ),
    Case(
        "number",
        "throw 42;\n",
    ),
    Case(
        "string",
        "throw \"hi\";\n",
    ),
]


@dataclass
class RunResult:
    exit_code: int
    stdout: str
    stderr: str
    timed_out: bool = False


def first_stderr_line(result: RunResult) -> str:
    lines = result.stderr.splitlines()
    return lines[0] if lines else ""


def run_case(cmd: list[str], case: Case, workdir: Path, timeout: float,
             env: dict | None = None) -> RunResult:
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
    return RunResult(exit_code=proc.returncode, stdout=proc.stdout,
                     stderr=proc.stderr)


def check_case(case: Case, native: RunResult, bootstrap: RunResult) -> list[str]:
    problems = []
    if native.timed_out:
        problems.append("native timed out -- corpus bug, fix the case")
        return problems
    if native.exit_code != 70:
        problems.append(
            f"native did not exit 70 (exit={native.exit_code}, stderr="
            f"{native.stderr!r}) -- corpus bug, fix the case")
        return problems
    if bootstrap.timed_out:
        problems.append("bootstrap timed out")
        return problems
    if bootstrap.exit_code != native.exit_code:
        problems.append(
            f"exit code: native={native.exit_code} bootstrap={bootstrap.exit_code}")
    native_line = first_stderr_line(native)
    bootstrap_line = first_stderr_line(bootstrap)
    if bootstrap_line != native_line:
        problems.append(
            f"first stderr line: native={native_line!r} bootstrap={bootstrap_line!r}")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--native", default=str(REPO_ROOT / "build" / "loxpp"))
    parser.add_argument("--bootstrap",
                        default=str(REPO_ROOT / "bootstrap" / "lox_wrapper.sh"))
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
            bootstrap_result = run_case([args.bootstrap], case, workdir,
                                        args.timeout, env=bootstrap_env)
            problems = check_case(case, native_result, bootstrap_result)
            if problems:
                failures.append(f"{case.name}:\n  " + "\n  ".join(problems))

    if failures:
        print(f"{len(failures)}/{len(CASES)} case(s) diverge:\n")
        print("\n\n".join(failures))
        return 1

    print(f"OK: {len(CASES)} uncaught-throw case(s) match between native and bootstrap.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
