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

CASES below covers both. check_defer_count_matches_depth() checks a count,
not a bool: every unwound call runs its own defer, not just one of them.
check_state_restore_probes() sweeps ambient depths looking for one that
actually interrupts stringify() mid-recursion, and checks stringifyDepth
came back to 0. check_state_after_examples() then runs the example corpus
with the interpreter's own state-dump hook enabled and asserts every
restored field is back at its start value after every example, not only
that the example exited 0.

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
    r"continueFlag=(true|false) throwFlag=(true|false) "
    r"maxStringifyDepthSeen=(\d+)$"
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


# "Each unwound call still runs its own pending defer" is a claim about a
# COUNT, not a bool: every one of the N frames the overflow unwound through
# must run its own defer, not just one of them. depth/noopRuns are compared
# by the check below instead of hard-coding the recursion's native ceiling,
# which shifts whenever the evaluator's own frame cost changes.
DEFER_COUNT_PROBE = (
    "var noopRuns = 0;\n"
    "fun noop() { noopRuns = noopRuns + 1; }\n"
    "var depth = 0;\n"
    "fun g(n) { defer noop(); depth = n; return g(n + 1); }\n"
    'try { g(0); } catch (e) { print "caught " + e.kind; }\n'
    'print "depth reached " + str(depth);\n'
    'print "defers ran " + str(noopRuns);\n'
)


def check_defer_count_matches_depth() -> list[str]:
    """Every unwound call runs its own pending defer, not just one of them."""
    failures = []
    stdout_lines, stderr_lines, exit_code = run(DEFER_COUNT_PROBE)
    if exit_code != 0:
        failures.append(
            f"defer count probe: exited {exit_code}, stdout {stdout_lines!r}, stderr {stderr_lines!r}"
        )
        return failures
    depth_m = next(
        (re.match(r"^depth reached (\d+)$", line) for line in stdout_lines if line.startswith("depth reached ")),
        None,
    )
    defers_m = next(
        (re.match(r"^defers ran (\d+)$", line) for line in stdout_lines if line.startswith("defers ran ")),
        None,
    )
    if not depth_m or not defers_m:
        failures.append(f"defer count probe: missing depth/defers output line(s): {stdout_lines!r}")
        return failures
    depth = int(depth_m.group(1))
    defers = int(defers_m.group(1))
    # g(0) through g(depth) is depth + 1 calls, each with its own defer.
    if defers != depth + 1:
        failures.append(
            f"defer count probe: recursion reached depth {depth} (depth+1={depth + 1} calls) "
            f"but only {defers} defer(s) ran -- expected one per unwound call, not just >0"
        )
    return failures


# Ambient call depths swept to find where native's own frame budget runs out
# WHILE stringify() is mid-recursion over the nested list, instead of
# stringify()'s own 100-level depth guard firing first (see
# notes/bootstrap-stack-depth.md) -- or so deep that native's budget is
# already exhausted before stringify() ever starts. A single fixed depth
# cannot tell those two apart from the restored-to-0 reading alone (both read
# stringifyDepth=0 on exit), and which depths land inside stringify() shifts
# whenever the evaluator's own native-frames-per-call cost changes -- exactly
# the kind of change this whole check exists to catch. Sweeping a wide band
# and checking maxStringifyDepthSeen (see the interpreter field of that name)
# keeps this probe meaningful even if that cost drifts.
STRINGIFY_DEPTH_SWEEP = [60, 80, 100, 110, 120, 130, 140, 150, 160, 170, 180, 200, 220, 240, 260]

# A run whose interruption never reached this deep into stringify()'s own
# recursion proves nothing about the restore -- see MIN_STRINGIFY_DEPTH_FLOOR
# below.
MIN_STRINGIFY_DEPTH_FLOOR = 20


def stringify_depth_probe(ambient_depth: int) -> str:
    return (
        "fun deep(d, v) { if (d == 0) { print v; return 0; } return deep(d - 1, v); }\n"
        "fun nest(n) { var v = []; var i = 0; while (i < n) { v = [v]; i = i + 1; } return v; }\n"
        "try {\n"
        f"    deep({ambient_depth}, nest(5000));\n"
        "} catch (e) {\n"
        "    print \"caught \" + e.kind;\n"
        "}\n"
    )


def check_state_restore_probes() -> list[str]:
    """Targeted probes for state a caught overflow must restore by hand,
    beyond what the ordinary example corpus happens to exercise.

    At a low ambient depth, stringify()'s OWN 100-level guard fires first.
    Since #325 that guard is fatal (fatalError -> exit(70)), matching
    native/JVM/CLR, so a run it wins is a clean halt with no
    __BOOTSTRAP_STATE__ line at all -- accepted the same way
    check_state_after_examples() accepts a fatal halt elsewhere in this
    file: exit 70/65, exactly one clean stderr line, no traceback. There is
    nothing to restore in that case -- the process exits before control
    ever returns to LoxFunction.call's own restore-on-StackOverflowError
    line. Only a run whose CAUGHT kind is StackOverflowError, with
    maxStringifyDepthSeen already above 0, proves native's own overflow
    landed while stringify() was mid-recursion -- the one case that
    actually exercises the restore.
    """
    failures = []
    best_max_depth_seen = 0
    exercised_the_restore = False
    for ambient_depth in STRINGIFY_DEPTH_SWEEP:
        stdout_lines, stderr_lines, exit_code = run(
            stringify_depth_probe(ambient_depth), {"LOXPP_BOOTSTRAP_CHECK_STATE": "1"}
        )
        if exit_code != 0:
            if exit_code not in (70, 65):
                failures.append(
                    f"stringifyDepth restore probe (D={ambient_depth}): exited {exit_code}, "
                    f"stdout {stdout_lines!r}, stderr {stderr_lines!r}"
                )
                continue
            if len(stderr_lines) != 1:
                failures.append(
                    f"stringifyDepth restore probe (D={ambient_depth}): fatal exit "
                    f"{exit_code} (stringify()'s own depth guard, #325) but stderr has "
                    f"{len(stderr_lines)} line(s), expected 1: {stderr_lines!r}"
                )
                continue
            if any("] in " in line for line in stdout_lines + stderr_lines):
                failures.append(
                    f"stringifyDepth restore probe (D={ambient_depth}): fatal exit "
                    f"{exit_code} but output has a native traceback: stdout "
                    f"{stdout_lines!r}, stderr {stderr_lines!r}"
                )
            continue
        state_lines = [m for m in (STATE_LINE_RE.match(line) for line in stdout_lines) if m]
        if not state_lines:
            failures.append(
                f"stringifyDepth restore probe (D={ambient_depth}): no __BOOTSTRAP_STATE__ line: "
                f"{stdout_lines!r}"
            )
            continue
        m = state_lines[-1]
        if m.group(2) != "0":
            failures.append(
                f"stringifyDepth restore probe (D={ambient_depth}): stringifyDepth={m.group(2)} "
                "after a caught overflow that interrupted stringify() mid-recursion (expected 0 -- "
                "a leaked value here corrupts every later print in the same process, not just this one)"
            )
        max_depth_seen = int(m.group(7))
        best_max_depth_seen = max(best_max_depth_seen, max_depth_seen)
        if "caught StackOverflowError" in stdout_lines and max_depth_seen >= MIN_STRINGIFY_DEPTH_FLOOR:
            exercised_the_restore = True
    if not exercised_the_restore:
        failures.append(
            f"stringifyDepth restore probe: no depth in the sweep {STRINGIFY_DEPTH_SWEEP} caught a "
            f"StackOverflowError while stringify() was already past depth {MIN_STRINGIFY_DEPTH_FLOOR} "
            f"(best maxStringifyDepthSeen seen anywhere={best_max_depth_seen}) -- either every run hit "
            "stringify()'s own depth guard (fatal, #325) first, or none reached stringify() at all, "
            "so a pass here would not prove the restore was ever exercised"
        )
    return failures


# examples/bench_jump_table.lox is a 5,000,000-iteration microbenchmark;
# interpreting it under the tree-walking bootstrap does not finish inside
# any reasonable per-example timeout (tools/check_clr_probes.sh and
# ci.yml's own CLR differential step exclude it by name for the same
# reason -- see the comment above that step).
EXCLUDED_EXAMPLES = {"bench_jump_table.lox"}


def check_state_after_examples(examples_dir: Path) -> list[str]:
    """Check state restoration and stdout identity per example, using native
    as ground truth.

    For each example, run it on native and bootstrap. Bootstrap's exit code
    must match native's. If bootstrap prints a state line, compare it to
    STATE_START. If bootstrap prints no state line, accept it only when
    both exit non-zero (70 or 65) with the same code, stderr is one line,
    and there is no traceback. Separately, bootstrap's stdout (with its own
    trailing __BOOTSTRAP_STATE__ line, which native never prints, removed
    first) must equal native's stdout exactly -- this is the only place a
    canonical-string-representation mismatch (issue #367: an enum value
    missing its enum name) would show up, since no other check compares
    plain stdout content between these two consumers.
    """
    failures = []
    native_bin = REPO_ROOT / "build" / "loxpp"
    if not native_bin.exists():
        return [f"native VM not found at {native_bin}"]

    examples = sorted(
        p for p in examples_dir.glob("*.lox") if p.name not in EXCLUDED_EXAMPLES
    )
    if not examples:
        return [f"no .lox files found under {examples_dir}"]

    for example in examples:
        input_file = example.with_suffix(".input")
        stdin_data = input_file.read_text() if input_file.exists() else None

        # Run on native
        try:
            native_result = subprocess.run(
                [str(native_bin), str(example)],
                capture_output=True,
                text=True,
                input=stdin_data,
                env=dict(os.environ, LANGUAGE="LOXPP"),
                timeout=60,
                check=False,
            )
        except subprocess.TimeoutExpired:
            failures.append(f"{example.name}: native timed out")
            continue

        # Run on bootstrap with state dump enabled
        env = dict(os.environ, LANGUAGE="LOXPP", LOXPP_BOOTSTRAP_CHECK_STATE="1")
        try:
            boot_result = subprocess.run(
                [str(WRAPPER), str(example)],
                capture_output=True,
                text=True,
                input=stdin_data,
                env=env,
                timeout=60,
                check=False,
            )
        except subprocess.TimeoutExpired:
            failures.append(f"{example.name}: bootstrap timed out")
            continue

        # Check: bootstrap exit code must equal native exit code
        if boot_result.returncode != native_result.returncode:
            failures.append(
                f"{example.name}: bootstrap exited {boot_result.returncode}, "
                f"native exited {native_result.returncode}"
            )
            continue

        # stdout content, with the state marker line (never printed by
        # native) removed, must match native's stdout exactly.
        boot_stdout_lines = boot_result.stdout.splitlines()
        boot_content_lines = [
            line for line in boot_stdout_lines if not STATE_LINE_RE.match(line)
        ]
        boot_content = "\n".join(boot_content_lines)
        native_content = native_result.stdout.rstrip("\n")
        if boot_content.rstrip("\n") != native_content:
            failures.append(
                f"{example.name}: bootstrap stdout does not match native "
                f"(state line excluded) -- native: {native_content[:200]!r}, "
                f"bootstrap: {boot_content[:200]!r}"
            )

        # If bootstrap printed a state line, check it
        state_lines = [
            m for m in (STATE_LINE_RE.match(line) for line in boot_stdout_lines) if m
        ]
        if state_lines:
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
        else:
            # No state line printed. This is OK only for fatal halts
            # matching native, with clean stderr.
            if boot_result.returncode not in (70, 65):
                # Not a fatal halt, should have printed state
                failures.append(
                    f"{example.name}: no __BOOTSTRAP_STATE__ line in stdout "
                    f"and exit code {boot_result.returncode} is not fatal (70 or 65)"
                )
                continue

            stderr_lines = boot_result.stderr.strip().split('\n') if boot_result.stderr.strip() else []
            if len(stderr_lines) != 1:
                failures.append(
                    f"{example.name}: no __BOOTSTRAP_STATE__ line and stderr "
                    f"has {len(stderr_lines)} lines, expected 1"
                )
                continue

            # Check for traceback
            has_traceback = "] in " in boot_result.stdout or "] in " in boot_result.stderr
            if has_traceback:
                failures.append(
                    f"{example.name}: no __BOOTSTRAP_STATE__ line but traceback found in output"
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
    failures += check_defer_count_matches_depth()
    failures += check_state_restore_probes()
    failures += check_state_after_examples(examples_dir)

    if failures:
        print(f"{len(failures)} bootstrap stack-overflow check failure(s):\n", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1
    checked = [p for p in examples_dir.glob("*.lox") if p.name not in EXCLUDED_EXAMPLES]
    print(
        f"OK: {len(CASES)} catchability/clean-stop cases, 1 defer-count probe, "
        f"{len(STRINGIFY_DEPTH_SWEEP)} stringifyDepth-restore sweep points, and "
        f"{len(checked)} example state-restore checks, all match."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
