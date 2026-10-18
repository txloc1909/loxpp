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
FATAL_FAST_PATH_PROBE = (
    REPO_ROOT / "test" / "translation-probes" / "58_defer_overflow_fatal_fast_path.lox"
)

PROBE_DIR = REPO_ROOT / "test" / "translation-probes"

# A second StackOverflowError raised by a deferred call while the first one
# unwinds is fatal, not delivered to the live catch (spec, "Runtime Errors").
# The probes are shared with the native VM and the JVM backend.
REENTRANT_OVERFLOW_PROBES = [
    "58_stack_overflow_reentrant_fatal",
    "defer_overflow_during_unwind",
    "defer_overflow_kind_spoof",
    "defer_replaces_unrelated_fault_during_unwind",
    "defer_throw_on_normal_return_during_unwind",
]

STATE_LINE_RE = re.compile(
    r"^__BOOTSTRAP_STATE__ defers=(\d+) stringifyDepth=(\d+) "
    r"returnFlag=(true|false) breakFlag=(true|false) "
    r"continueFlag=(true|false) throwFlag=(true|false) "
    r"maxStringifyDepthSeen=(\d+) "
    r"overflowUnwinding=(true|false) overflowDeferNest=(\d+)$"
)
STATE_START = {
    "defers": "0",
    "stringifyDepth": "0",
    "returnFlag": "false",
    "breakFlag": "false",
    "continueFlag": "false",
    "throwFlag": "false",
    "overflowUnwinding": "false",
    "overflowDeferNest": "0",
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
        "fatal fast path: no try anywhere, the pending defer of the faulting call does not run",
        "fun rec(n) { return rec(n + 1) + 1; }\n"
        'fun say() { print "main defer"; }\n'
        "fun main_fn() { defer say(); rec(0); }\n"
        "main_fn();\n",
        [],
        ["Stack overflow."],
        70,
    ),
    (
        "fatal fast path: a defer in every unwound frame is skipped (probe 58)",
        FATAL_FAST_PATH_PROBE.read_text(),
        [],
        ["Stack overflow."],
        70,
    ),
    (
        "fatal fast path: spawned task, no try in the task",
        "fun rec(n) { return rec(n + 1) + 1; }\n"
        'fun say() { print "task defer"; }\n'
        "fun worker() { defer say(); rec(0); }\n"
        "task.spawn(worker);\n"
        "task.receive();\n",
        [],
        ["Stack overflow."],
        70,
    ),
    (
        "fatal fast path: a try in the main task does not cover a spawned task",
        "fun rec(n) { return rec(n + 1) + 1; }\n"
        'fun say() { print "task defer"; }\n'
        "fun worker() { defer say(); rec(0); }\n"
        "try {\n"
        "    task.spawn(worker);\n"
        "    task.receive();\n"
        '} catch (e) { print "caught " + e.kind; }\n',
        [],
        ["Stack overflow."],
        70,
    ),
    (
        "fatal fast path: coroutine, no try in its resumer",
        "fun rec(n) { return rec(n + 1) + 1; }\n"
        'fun say() { print "co defer"; }\n'
        "fun body() { defer say(); rec(0); }\n"
        "coroutine.create(body).resume();\n"
        'print "after";\n',
        [],
        ["Stack overflow."],
        70,
    ),
    (
        "fatal fast path: a try inside a suspended coroutine does not cover its next resume",
        "fun rec(n) { return rec(n + 1) + 1; }\n"
        'fun say() { print "co defer"; }\n'
        "fun body() {\n"
        "    defer say();\n"
        '    try { yield 1; } catch (e) { print "no"; }\n'
        "    rec(0);\n"
        "}\n"
        "var co = coroutine.create(body);\n"
        "co.resume();\n"
        "co.resume();\n"
        'print "after";\n',
        [],
        ["Stack overflow."],
        70,
    ),
    (
        "fatal fast path: the catch block is not under its own try",
        "fun rec(n) { return rec(n + 1) + 1; }\n"
        'fun say() { print "d"; }\n'
        "fun f() { defer say(); try { throw 1; } catch (e) { rec(0); } }\n"
        "f();\n",
        [],
        ["Stack overflow."],
        70,
    ),
    (
        "fatal fast path: a caught overflow leaves no try block counted behind it",
        "fun rec(n) { return rec(n + 1) + 1; }\n"
        "fun say(s) { print s; }\n"
        "fun r(n) { try { return r(n + 1); } catch (e) { return e.kind; } }\n"
        'fun m() { defer say("main defer"); print r(0); rec(0); }\n'
        "m();\n",
        ["StackOverflowError"],
        ["Stack overflow."],
        70,
    ),
    (
        "fatal fast path: a task that wakes from a receive inside a try gets back its own count",
        "fun rec(n) { return rec(n + 1) + 1; }\n"
        "fun say(s) { print s; }\n"
        'fun worker(p) { task.send(p, "hi"); }\n'
        "var me = task.self();\n"
        "fun m() {\n"
        '    defer say("main defer");\n'
        '    try { task.spawn(fun_w); print task.receive(); } catch (e) { print "no"; }\n'
        "    rec(0);\n"
        "}\n"
        "fun fun_w() { worker(me); }\n"
        "m();\n",
        ["hi"],
        ["Stack overflow."],
        70,
    ),
    (
        "catchable: a try in the same function sees the overflow; the defer of that function runs after the catch",
        "fun rec(n) { return rec(n + 1) + 1; }\n"
        'fun say() { print "d"; }\n'
        'fun f() { try { defer say(); rec(0); } catch (e) { print "caught " + e.kind; } }\n'
        "f();\n"
        'print "after";\n',
        ["caught StackOverflowError", "d", "after"],
        [],
        0,
    ),
    (
        "catchable: the resumer's try covers a coroutine that overflows",
        "fun rec(n) { return rec(n + 1) + 1; }\n"
        'fun say() { print "d"; }\n'
        "fun body() { defer say(); yield 1; rec(0); }\n"
        "var co = coroutine.create(body);\n"
        "co.resume();\n"
        'try { co.resume(); } catch (e) { print "caught " + e.kind; }\n'
        'print "after";\n',
        ["d", "caught StackOverflowError", "after"],
        [],
        0,
    ),
    *[
        (
            f"fatal: a second overflow during an overflow unwind is not caught ({name})",
            (PROBE_DIR / f"{name}.lox").read_text(),
            [],
            ["Stack overflow."],
            70,
        )
        for name in REENTRANT_OVERFLOW_PROBES
    ],
    (
        "catchable: two overflows one after the other are both delivered",
        "fun f(n) { return 1 + f(n + 1); }\n"
        'try { f(0); } catch (e) { print "first " + e.kind; }\n'
        'try { f(0); } catch (e) { print "second " + e.kind; }\n'
        'print "after";\n',
        ["first StackOverflowError", "second StackOverflowError", "after"],
        [],
        0,
    ),
    (
        "catchable: a throw that replaces the overflow in a defer ends the unwind when caught",
        'fun thrower() { throw "replaced"; }\n'
        "fun f(n) { defer thrower(); f(n + 1); }\n"
        'try { f(0); } catch (e) { print "outer " + str(e); }\n'
        "fun h(n) { return 1 + h(n + 1); }\n"
        'try { h(0); } catch (e) { print "again " + e.kind; }\n'
        'print "after";\n',
        ["outer replaced", "again StackOverflowError", "after"],
        [],
        0,
    ),
    (
        "fatal: a catch that ended an overflow unwind does not hide a later re-entrant overflow",
        "fun h(n) { return 1 + h(n + 1); }\n"
        'fun f(n) { try { h(n); } catch (e) { print "in " + e.kind; } }\n'
        "f(0);\n"
        'try { h(0); } catch (e) { print "again " + e.kind; }\n'
        "fun k(n) { defer f(n); k(n + 1); }\n"
        'try { k(0); } catch (e) { print "k " + e.kind; }\n'
        'print "end";\n',
        ["in StackOverflowError", "again StackOverflowError"],
        ["Stack overflow."],
        70,
    ),
    # Only the frames n < 20 hold a defer, so the first deferred call runs
    # with plenty of native frames left and a try/catch inside it completes.
    # That inner catch must not end the unwind: deepcall(2000) overflows
    # again, and the budget of native is below 2000 frames.
    (
        "fatal: a try/catch inside a deferred call does not end the overflow unwind",
        "class Fake {}\n"
        "fun deepcall(n) { if (n == 0) return 0; return 1 + deepcall(n - 1); }\n"
        'fun boom(n) { try { throw Fake(); } catch (e) { print "local"; } deepcall(2000); }\n'
        "fun f(n) { if (n < 20) defer boom(n); f(n + 1); }\n"
        'try { f(0); } catch (e) { print "caught " + e.kind; }\n'
        'print "after";\n',
        ["local"],
        ["Stack overflow."],
        70,
    ),
    (
        "catchable: the same deferred calls without a second overflow run, then the catch gets the first",
        "class Fake {}\n"
        "fun deepcall(n) { if (n == 0) return 0; return 1 + deepcall(n - 1); }\n"
        'fun boom(n) { try { throw Fake(); } catch (e) { print "local"; } deepcall(5); }\n'
        "fun f(n) { if (n < 20) defer boom(n); f(n + 1); }\n"
        'try { f(0); } catch (e) { print "caught " + e.kind; }\n'
        'print "after";\n',
        ["local"] * 20 + ["caught StackOverflowError", "after"],
        [],
        0,
    ),
    (
        "parse-time: deeply nested grouping is a compile error, not a crash",
        'print ' + '(' * 400 + '1' + ')' * 400 + ';\n',
        [],
        ["[line 0] Error: Stack overflow."],
        65,
    ),
]


# Programs that native completes and the bootstrap halts on. Issue #687: the
# bootstrap has no frame reserve for a deferred call that runs during an
# overflow unwind. Issue #716: the bootstrap does not run a self tail call in
# constant call depth. Each entry pins the gap: native must finish
# with exit 0, and the bootstrap must halt with one "Stack overflow." line.
# When a fix lands, an entry starts to match native and
# check_known_gaps() fails; move the entry to CASES then.
def _gap_program(deferred_body: str, extra: str = "") -> str:
    return (
        extra
        + "var ran = 0;\n"
        + f"fun boom(n) {{ {deferred_body} }}\n"
        + "fun f(n) { defer boom(n); f(n + 1); }\n"
        + 'try { f(0); } catch (e) { print "caught " + e.kind; }\n'
        + 'print "ran>0 " + str(ran > 0);\n'
        + 'print "after";\n'
    )


KNOWN_GAPS = [
    (
        "5000 self tail calls (the two CASES that recurse with `return f(n + 1);` rely on this)",
        "fun f(n) {\n"
        "    if (n == 0) return 0;\n"
        "    return f(n - 1);\n"
        "}\n"
        "f(5000);\n"
        'print "after";\n',
        716,
    ),
    ("a deferred call with one assignment", _gap_program("ran = ran + 1;"), 687),
    (
        "a deferred call that calls one helper",
        _gap_program("helper();", "fun helper() { ran = ran + 1; }\n"),
        687,
    ),
    (
        "a deferred call with a local try/catch",
        _gap_program("try { throw Fake(); } catch (e) { ran = ran + 1; }", "class Fake {}\n"),
        687,
    ),
    (
        "a deferred call that calls deepcall(5)",
        _gap_program(
            "deepcall(5); ran = ran + 1;",
            "fun deepcall(n) { if (n == 0) return 0; return 1 + deepcall(n - 1); }\n",
        ),
        687,
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


def check_known_gaps() -> list[str]:
    """Each KNOWN_GAPS program still differs from native in the pinned way."""
    failures = []
    native_bin = REPO_ROOT / "build" / "loxpp"
    if not native_bin.exists():
        return [f"native VM not found at {native_bin}"]
    for name, program, issue in KNOWN_GAPS:
        with tempfile.NamedTemporaryFile("w", suffix=".lox", delete=False) as f:
            f.write(program)
            path = f.name
        try:
            native = subprocess.run(
                [str(native_bin), path], capture_output=True, text=True, timeout=60, check=False
            )
        finally:
            os.unlink(path)
        if native.returncode != 0 or native.stdout.splitlines()[-1:] != ["after"]:
            failures.append(
                f"known gap #{issue} ({name}): native no longer completes: "
                f"exit {native.returncode}, stdout {native.stdout!r}"
            )
            continue
        stdout_lines, stderr_lines, exit_code = run(program)
        if stdout_lines == native.stdout.splitlines() and exit_code == 0:
            failures.append(
                f"known gap #{issue} ({name}): the bootstrap now matches native; "
                "move this entry from KNOWN_GAPS to CASES"
            )
        elif (stdout_lines, stderr_lines, exit_code) != ([], ["Stack overflow."], 70):
            failures.append(
                f"known gap #{issue} ({name}): expected the pinned halt, got exit {exit_code}, "
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
# docs/reference/bootstrap-stack-depth.md) -- or so deep that native's budget is
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
    native/JVM, so a run it wins is a clean halt with no
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
# interpreting it under the tree-walking bootstrap does not finish inside any
# reasonable per-example timeout.
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
                "overflowUnwinding": m.group(8),
                "overflowDeferNest": m.group(9),
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
    failures += check_known_gaps()
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
        f"{len(KNOWN_GAPS)} pinned known gaps, {len(STRINGIFY_DEPTH_SWEEP)} stringifyDepth-restore sweep points, and "
        f"{len(checked)} example state-restore checks, all match."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
