#!/usr/bin/env python3
"""
Differential check: bootstrap's Resolver compile-time errors against
native's compiler (issue #410).

The bootstrap interpreter's `Resolver` (bootstrap/loxpp_interpreter.lox)
reports several static errors that mirror native's compiler (src/compiler.cpp):
`break`/`continue` outside a loop, `return` outside a function, and `defer`
outside a function. Nothing had run these through both consumers and
compared the result, so a wording or exit-code drift between the two could
go unnoticed indefinitely.

Two things that make an ordinary stdout diff (tools/diff_runtimes.py) the
wrong tool here:

  - Native writes a compile error to stderr, exit code 65, with empty
    stdout. bootstrap/lox_wrapper.sh's own protocol relays the interpreted
    program's LOXERR65 line to *its* stderr and sets *its* own exit code to
    65 (see the wrapper's own comments), so the two are directly comparable
    -- but only on stderr, which diff_runtimes.py never looks at.
  - A release build is required: a debug build's instruction trace
    (LOXPP_DEBUG_TRACE_EXECUTION/LOXPP_DEBUG_PRINT_CODE) floods native's
    stdout with a disassembly of bootstrap/loxpp_interpreter.lox itself
    (native is the interpreter that runs the bootstrap script), which would
    not corrupt this check's stderr-only comparison, but the same trace
    macros currently gate every other bootstrap ctest entry that runs a
    program through the wrapper (BootstrapErrorKindCausesRuntime,
    BootstrapStackOverflow) toward skipping on a debug build instead of
    trusting a comparison against noise. This script follows the same rule
    for consistency, via --trace-enabled / LOXPP_BOOTSTRAP_TRACE_ENABLED.

Native's own stderr and exit code are the ground truth for each case --
there is no separately maintained "expected message" string to fall out of
sync with src/compiler.cpp's wording. A case whose native run does not
itself exit 65 with non-empty stderr is a corpus bug (the case no longer
triggers a compile error at all) and is reported as a failure, not silently
skipped.

Usage:
    tools/check_bootstrap_resolver_errors.py [--native PATH] [--bootstrap PATH]
                                              [--timeout SECONDS]

Exits 0 when every case's bootstrap run matches native's exit code and
stderr text exactly. Exits 1 and prints every mismatch otherwise. Exits 125
(ctest's SKIP_RETURN_CODE) when --trace-enabled says the binary under test
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


@dataclass
class Case:
    name: str
    program: str  # full source text, including trailing newline


# One case per resolver check issue #410 names. Each program is the
# smallest one that reaches the check with nothing else in the file to
# confuse the line number or produce a second diagnostic.
CASES = [
    Case("break_outside_loop", "break;\n"),
    Case("continue_outside_loop", "continue;\n"),
    Case("return_outside_function", "return 1;\n"),
    # `nothing` need not exist: both compilers reject the statement before
    # ever asking whether the callee resolves to anything.
    Case("defer_outside_function", "defer nothing();\n"),
]


@dataclass
class RunResult:
    exit_code: int
    stderr: str
    stdout: str
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
        return RunResult(exit_code=-1, stderr="", stdout="", timed_out=True)
    return RunResult(exit_code=proc.returncode, stderr=proc.stderr.strip(), stdout=proc.stdout.strip())


def check_case(case: Case, native: RunResult, bootstrap: RunResult) -> list[str]:
    problems = []
    if native.timed_out:
        problems.append("native timed out -- corpus bug, fix the case")
        return problems
    if native.exit_code != 65 or not native.stderr:
        problems.append(
            f"native did not report a compile error (exit={native.exit_code}, "
            f"stderr={native.stderr!r}) -- corpus bug, fix the case"
        )
        return problems
    if bootstrap.timed_out:
        problems.append("bootstrap timed out")
        return problems
    if bootstrap.exit_code != native.exit_code:
        problems.append(f"exit code: native={native.exit_code} bootstrap={bootstrap.exit_code}")
    if bootstrap.stderr != native.stderr:
        problems.append(f"message: native={native.stderr!r} bootstrap={bootstrap.stderr!r}")
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

    print(f"OK: {len(CASES)} resolver-error case(s) match between native and bootstrap.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
