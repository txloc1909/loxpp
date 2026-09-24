#!/usr/bin/env python3
"""
Differential check: bootstrap's match-arm block body against native's
(issue #434).

Before this fix, bootstrap's `Parser.matchExpression`
(bootstrap/loxpp_interpreter.lox) parsed every arm body as a single
`expression()` and never accepted the block form
(`"{" declaration* expression "}"`) spec/02-syntax.md's `armBody` rule
allows. `break`/`continue`/`return` statements inside an arm's `{ }` block
read as expressions and failed to parse at all -- not just a resolver
message gap (that part was issue #410).

CASES below covers the block form end to end: parsing, the resolver's
`break`-is-legal-with-no-enclosing-loop / `continue`-still-needs-a-real-
loop split, and the interpreter's break/continue/return handling inside an
arm. Each case's *expected* stdout, stderr, and exit code come from
running native -- there is no separately maintained "expected output"
string to fall out of sync with src/compiler.cpp's behavior.

Same trace-noise and release-build constraints as
tools/check_bootstrap_resolver_errors.py (see that file's docstring): a
debug build's LOXPP_DEBUG_TRACE_EXECUTION/LOXPP_DEBUG_PRINT_CODE floods
native's stdout with a disassembly of bootstrap/loxpp_interpreter.lox
itself, so this check skips itself (ctest's SKIP_RETURN_CODE) rather than
compare against noise.

Usage:
    tools/check_bootstrap_match_arm_break.py [--native PATH] [--bootstrap PATH]
                                              [--timeout SECONDS]

Exits 0 when every case's bootstrap run matches native's stdout, stderr,
and exit code. Exits 1 and prints every mismatch otherwise. Exits 125
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


CASES = [
    Case(
        "break_with_no_enclosing_loop",
        """\
fun f() {
    match 1 {
        case 1 => { break; }
        case _ => 3
    };
    print "after";
}
f();
""",
    ),
    Case(
        "break_inside_loop_exits_only_match",
        """\
fun f() {
    var count = 0;
    var i = 0;
    while (i < 3) {
        i = i + 1;
        match i {
            case 2 => { break; }
            case _ => count = count + 1
        };
    }
    print count;
}
f();
""",
    ),
    Case(
        "continue_in_match_continues_enclosing_loop",
        """\
fun f() {
    var i = 0;
    while (i < 5) {
        i = i + 1;
        match i {
            case 3 => { continue; }
            case _ => nil
        };
        print i;
    }
}
f();
""",
    ),
    Case(
        "return_in_match_arm_exits_function",
        """\
fun f(x) {
    match x {
        case 1 => { return "one"; }
        case _ => "other"
    };
    return "unreachable";
}
print f(1);
print f(2);
""",
    ),
    Case(
        "var_decl_in_match_arm_block",
        """\
fun f(x) {
    return match x {
        case 1 => { var y = x + 1; y * 2 }
        case _ => 0
    };
}
print f(1);
""",
    ),
    Case(
        "plain_expression_arm_still_works",
        """\
fun f(x) {
    return match x {
        case 1 => "one"
        case _ => "other"
    };
}
print f(1);
print f(9);
""",
    ),
    # Negative cases: matchArmDepth must make `break` legal with no loop,
    # but must not also make `continue` legal -- it still has to reach a
    # real enclosing loop (spec/04-semantics.md).
    Case(
        "continue_in_match_no_enclosing_loop_is_error",
        """\
match 1 {
    case 1 => { continue; }
    case _ => 2
};
""",
    ),
    Case(
        "match_arm_block_without_trailing_expression_is_error",
        """\
match 1 {
    case 1 => { var y = 1; }
    case _ => 2
};
""",
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
    if bootstrap.exit_code != native.exit_code:
        problems.append(f"exit code: native={native.exit_code} bootstrap={bootstrap.exit_code}")
    if bootstrap.stdout != native.stdout:
        problems.append(f"stdout: native={native.stdout!r} bootstrap={bootstrap.stdout!r}")
    if bootstrap.stderr != native.stderr:
        problems.append(f"stderr: native={native.stderr!r} bootstrap={bootstrap.stderr!r}")
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

    print(f"OK: {len(CASES)} match-arm-break case(s) match between native and bootstrap.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
