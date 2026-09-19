#!/usr/bin/env python3
"""
Checks the bootstrap interpreter's (bootstrap/loxpp_interpreter.lox) own
handling of a target program's deep recursion: a StackOverflowError raised
by the native VM running the interpreter must reach the target program the
same way any other runtime fault does, and must never corrupt the
interpreter's own state for programs run afterward.

Two things a diff of stdout cannot tell apart:

- "The example passed" is not "the interpreter's own state came back to
  where it started." A StackOverflowError unwinds through the native VM,
  skipping whatever bootstrap/loxpp_interpreter.lox's own LoxFunction.call
  would otherwise restore on its normal return path (pendingDefers,
  stringifyDepth, the return flags). A leaked value does not usually break
  the program that leaks it -- it breaks whatever the SAME process runs
  next, so a plain pass/fail per example can miss it entirely.
- A caught fault and an uncaught one must produce different output (a
  `.kind`/`.message` pair delivered to a `catch`, versus a single clean
  `Stack overflow.` line and no native traceback) for the same underlying
  event.

CASES below covers both. STATE_PROBES then runs the example corpus with the
interpreter's own state-dump hook enabled and asserts every field is back at
its start value after every example, not only that the example exited 0.

Usage:
    tools/check_bootstrap_stack_overflow.py [<examples-dir>]

<examples-dir> defaults to examples/. Exits 125 (ctest's configured
SKIP_RETURN_CODE) without running anything when
LOXPP_BOOTSTRAP_TRACE_ENABLED=1 is set -- see
tools/run_bootstrap_error_kind_causes.py's docstring for why a debug-preset
build cannot run this check.
"""

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
WRAPPER = REPO_ROOT / "bootstrap" / "lox_wrapper.sh"

STATE_LINE_RE = re.compile(
    r"^__BOOTSTRAP_STATE__ defers=(\d+) stringifyDepth=(\d+) "
    r"returnFlag=(true|false) breakFlag=(true|false) "
    r"continueFlag=(true|false) throwFlag=(true|false)$"
)
STATE_START = {
    "defers": "0",
    "stringifyDepth": "0",
    "returnFlag": "false",
    "breakFlag": "false",
    "continueFlag": "false",
    "throwFlag": "false",
}

# (name, program, expected stdout lines, expected stderr lines, expected
# exit code). bootstrap/lox_wrapper.sh reroutes a LOXERR65/LOXERR70-prefixed
# line to stderr with the prefix stripped (its own controlled-halt
# protocol); everything else it passes through to stdout unchanged.
CASES = [
    (
        "catchable: target try/catch sees the spec's kind and message",
        'fun f(n) { return f(n + 1); }\n'
        'try {\n'
        '    f(0);\n'
        '    print "no error";\n'
        '} catch (e) {\n'
        '    print "caught " + e.kind;\n'
        '    print e.message;\n'
        '}\n'
        'print "after";\n',
        ["caught StackOverflowError", "Stack overflow.", "after"],
        [],
        0,
    ),
    (
        "uncaught: clean stop, no target try -- one message line, no traceback",
        'fun f(n) { return f(n + 1); }\n'
        'f(0);\n',
        [],
        ["Stack overflow."],
        70,
    ),
    (
        "parse-time: deeply nested grouping is a compile error, not a crash",
        'print ' + '(' * 400 + '1' + ')' * 400 + ';\n',
        [],
        ["[line 0] Error: Stack overflow."],
        65,
    ),
    (
        "each unwound call still runs its own pending defer",
        'var noopRuns = 0;\n'
        'fun noop() { noopRuns = noopRuns + 1; }\n'
        'fun f(n) {\n'
        '    defer noop();\n'
        '    return f(n + 1);\n'
        '}\n'
        'try {\n'
        '    f(0);\n'
        '} catch (e) {\n'
        '    print "caught " + e.kind;\n'
        '}\n'
        'print noopRuns > 0;\n',
        ["caught StackOverflowError", "true"],
        [],
        0,
    ),
]


def run(program: str, extra_env: dict | None = None) -> tuple[list[str], list[str], int]:
    with tempfile.NamedTemporaryFile("w", suffix=".lox", delete=False) as f:
        # A source file with no trailing newline mis-lexes the final token
        # at EOF (a pre-existing scanner quirk unrelated to this check).
        f.write(program + "\n")
        path = f.name
    try:
        env = dict(os.environ, LANGUAGE="LOXPP")
        if extra_env:
            env.update(extra_env)
        result = subprocess.run(
            [str(WRAPPER), path],
            capture_output=True,
            text=True,
            env=env,
            timeout=60,
            check=False,
        )
        return result.stdout.splitlines(), result.stderr.splitlines(), result.returncode
    finally:
        os.unlink(path)


def check_cases() -> list[str]:
    failures = []
    for name, program, expected_stdout, expected_stderr, expected_exit in CASES:
        stdout_lines, stderr_lines, exit_code = run(program)
        if exit_code != expected_exit:
            failures.append(
                f"{name}: exited {exit_code} (expected {expected_exit}), "
                f"stdout {stdout_lines!r}, stderr {stderr_lines!r}"
            )
            continue
        if stdout_lines != expected_stdout:
            failures.append(f"{name}: expected stdout {expected_stdout!r}, got {stdout_lines!r}")
        if stderr_lines != expected_stderr:
            failures.append(f"{name}: expected stderr {expected_stderr!r}, got {stderr_lines!r}")
        if any("] in " in line for line in stdout_lines + stderr_lines):
            failures.append(
                f"{name}: a native traceback line leaked into output: "
                f"stdout {stdout_lines!r}, stderr {stderr_lines!r}"
            )
    return failures


# Ambient call depth deep enough that native's own frame budget runs out
# WHILE stringify() is mid-recursion over the nested list, instead of
# stringify()'s own 100-level depth guard firing first (see
# notes/bootstrap-stack-depth.md) -- but not so deep that native's budget is
# already exhausted before stringify() ever starts (empirically, well past
# this ambient depth the interruption lands at stringifyDepth 0 or 1, too
# shallow to distinguish "restored" from "never got that high" either way).
# This is the one shape the example corpus does not naturally exercise: an
# ordinary program's ambient call depth at a print site stays far short of
# this. Caught this way empirically -- with the restore removed, this exact
# probe leaves stringifyDepth at 53, not 0.
STRINGIFY_DEPTH_RESTORE_PROBE = (
    "fun deep(d, v) { if (d == 0) { print v; return 0; } return deep(d - 1, v); }\n"
    "fun nest(n) { var v = []; var i = 0; while (i < n) { v = [v]; i = i + 1; } return v; }\n"
    "try {\n"
    "    deep(150, nest(5000));\n"
    "} catch (e) {\n"
    "    print \"caught \" + e.kind;\n"
    "}\n"
)


def check_state_restore_probes() -> list[str]:
    """Targeted probes for state a caught overflow must restore by hand,
    beyond what the ordinary example corpus happens to exercise."""
    failures = []
    stdout_lines, _stderr_lines, exit_code = run(
        STRINGIFY_DEPTH_RESTORE_PROBE, {"LOXPP_BOOTSTRAP_CHECK_STATE": "1"}
    )
    if exit_code != 0:
        failures.append(f"stringifyDepth restore probe: exited {exit_code}, stdout {stdout_lines!r}")
        return failures
    state_lines = [m for m in (STATE_LINE_RE.match(line) for line in stdout_lines) if m]
    if not state_lines:
        failures.append(f"stringifyDepth restore probe: no __BOOTSTRAP_STATE__ line: {stdout_lines!r}")
        return failures
    m = state_lines[-1]
    if m.group(2) != "0":
        failures.append(
            f"stringifyDepth restore probe: stringifyDepth={m.group(2)} after a caught overflow "
            "that interrupted stringify() mid-recursion (expected 0 -- a leaked value here "
            "corrupts every later print in the same process, not just this one)"
        )
    return failures


# examples/bench_jump_table.lox is a 5,000,000-iteration microbenchmark;
# interpreting it under the tree-walking bootstrap does not finish inside
# any reasonable per-example timeout (tools/check_clr_probes.sh and
# ci.yml's own CLR differential step exclude it by name for the same
# reason -- see the comment above that step).
EXCLUDED_EXAMPLES = {"bench_jump_table.lox"}


def check_state_after_examples(examples_dir: Path) -> list[str]:
    failures = []
    examples = sorted(
        p for p in examples_dir.glob("*.lox") if p.name not in EXCLUDED_EXAMPLES
    )
    if not examples:
        return [f"no .lox files found under {examples_dir}"]
    for example in examples:
        env = dict(os.environ, LANGUAGE="LOXPP", LOXPP_BOOTSTRAP_CHECK_STATE="1")
        input_file = example.with_suffix(".input")
        stdin_data = input_file.read_text() if input_file.exists() else None
        try:
            result = subprocess.run(
                [str(WRAPPER), str(example)],
                capture_output=True,
                text=True,
                input=stdin_data,
                env=env,
                timeout=60,
                check=False,
            )
        except subprocess.TimeoutExpired:
            failures.append(f"{example.name}: timed out")
            continue
        state_lines = [
            m for m in (STATE_LINE_RE.match(line) for line in result.stdout.splitlines()) if m
        ]
        if not state_lines:
            failures.append(f"{example.name}: no __BOOTSTRAP_STATE__ line in stdout")
            continue
        m = state_lines[-1]
        actual = {
            "defers": m.group(1),
            "stringifyDepth": m.group(2),
            "returnFlag": m.group(3),
            "breakFlag": m.group(4),
            "continueFlag": m.group(5),
            "throwFlag": m.group(6),
        }
        if actual != STATE_START:
            failures.append(
                f"{example.name}: interpreter state not restored after run: {actual}"
            )
    return failures


def main() -> int:
    if os.environ.get("LOXPP_BOOTSTRAP_TRACE_ENABLED") == "1":
        print(
            "SKIP: build/loxpp was built with LOXPP_DEBUG_TRACE_EXECUTION or "
            "LOXPP_DEBUG_PRINT_CODE on, so its stdout is a per-instruction trace, "
            "not the plain lines this test reads. Build with the release preset "
            "(both flags off) to run this test for real.",
            file=sys.stderr,
        )
        return 125

    examples_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO_ROOT / "examples"

    failures = check_cases()
    failures += check_state_restore_probes()
    failures += check_state_after_examples(examples_dir)

    if failures:
        print(f"{len(failures)} bootstrap stack-overflow check failure(s):\n", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1
    checked = [p for p in examples_dir.glob("*.lox") if p.name not in EXCLUDED_EXAMPLES]
    print(
        f"OK: {len(CASES)} catchability/clean-stop cases and "
        f"{len(checked)} example state-restore checks, all match."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
