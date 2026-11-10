#!/usr/bin/env python3
"""Checks that tools/diff_runtimes.py compares both phases: whether a program
is rejected at compile time, and what it does when it runs (AGENTS.md, "Plans
for a language change", rule 5).

Each case runs tools/diff_runtimes.py with the native binary on one side and a
stub runner on the other. A stub stands for a consumer with one defect: it
rejects a program, it accepts one, or it prints other output. The case states
the outcome that the gate must report. A control case runs native against
itself on every program, so no case passes only because a program cannot run.

Cases:
  1. native rejects a program and the stub runs it: DIVERGE, exit 1.
  2. native runs a program and the stub rejects it: DIVERGE, exit 1.
  3. native halts after the CHECK lines of a `STATUS: error` probe and the
     stub rejects it (empty stdout, non-zero status): DIVERGE, exit 1. An
     empty stdout is a prefix of every stdout, so a prefix rule alone reports
     MATCH (halt) here, and run-time parity hides a compile-time difference.
  4. native runs a program and the stub prints one more line: DIVERGE, exit 1.
  5. native runs a program and the stub prints the same stdout with another
     exit status: DIVERGE, exit 1.
  6. native halts after the CHECK lines and the stub halts with the same lines
     and one more (a deferred call that only it runs): MATCH (halt), exit 0.
  7. control: native against itself on every program above: exit 0.
  8. native halts on a program that is not a spec probe, the stub prints
     nothing and exits non-zero, and the `--other-check` runner rejects the
     program: DIVERGE (compile phase), exit 1.
  9. the same halt, and the `--other-check` runner accepts the program: the
     empty stdout is a real halt, MATCH (halt), exit 0.
 10. the same halt, and the `--other-check` runner does not finish: DIVERGE,
     exit 1.
 11. the same halt with no `--other-check`: the check never runs, so the
     silent halt is MATCH (halt), exit 0.
 12. the same halt with `--other-check`, and the other runner prints output: the
     check does not run, so the output decides, MATCH (halt), exit 0.

Usage:
    tools/check_diff_runtimes_phases.py --native build/loxpp

Exits 0 when every case holds, 1 with a report per failed case, 2 when a
precondition fails, and 125 when the build traces execution (its stdout then
holds the trace, so no stdout comparison can pass).
"""

import argparse
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DIFF_RUNTIMES = REPO_ROOT / "tools" / "diff_runtimes.py"

sys.path.insert(0, str(Path(__file__).resolve().parent))

from runner_precondition import refuse_unrunnable, require_runner

STATIC_ERROR = """print "before";
break;
"""
RUNS = """print "one";
print "two";
"""
HALT_PROBE = """print "before";
print undeclared_name;
print "after";
// STATUS: error
// CHECK: before
"""
# Not a spec probe: a halt before any output, which a rejection also looks like.
HALT_PLAIN = """print undeclared_name;
"""


def write_stub(path: Path, body: str) -> Path:
    path.write_text("#!/bin/sh\n" + body, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return path


def diff(native: str, runner: str, programs: list[Path], extra: tuple[str, ...] = ()) -> tuple[int, str]:
    completed = subprocess.run(
        [sys.executable, str(DIFF_RUNTIMES), native, runner, *extra, *map(str, programs)],
        capture_output=True,
        text=True,
        timeout=300,
    )
    return completed.returncode, completed.stdout + completed.stderr


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", required=True)
    args = parser.parse_args()
    require_runner(args.native)
    native = str(Path(args.native).resolve())

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        static_error = work / "static_error.lox"
        static_error.write_text(STATIC_ERROR, encoding="utf-8")
        runs = work / "runs.lox"
        runs.write_text(RUNS, encoding="utf-8")
        halt_probe = work / "halt_probe.lox"
        halt_probe.write_text(HALT_PROBE, encoding="utf-8")
        halt_plain = work / "halt_plain.lox"
        halt_plain.write_text(HALT_PLAIN, encoding="utf-8")

        try:
            trace = subprocess.run([native, str(runs)], capture_output=True, text=True, timeout=60)
        except OSError as exc:
            refuse_unrunnable(native, exc)
        if trace.stdout != "one\ntwo\n":
            print("SKIP: the build under test does not print exactly the program's stdout "
                  "(a trace build); no stdout comparison can pass")
            return 125

        rejector = write_stub(
            work / "rejector.sh", 'echo "stub: rejected" >&2\nexit 1\n'
        )
        acceptor = write_stub(work / "acceptor.sh", 'echo "before"\nexit 0\n')
        extra_line = write_stub(
            work / "extra_line.sh", f'"{native}" "$1"\necho "extra"\n'
        )
        other_status = write_stub(
            work / "other_status.sh", f'"{native}" "$1"\nexit 3\n'
        )
        halt_with_defer = write_stub(
            work / "halt_with_defer.sh", 'echo "before"\necho "deferred"\nexit 1\n'
        )
        # The run prints nothing and exits 1. Under LOXPP_CHECK_ONLY=1 (the
        # --other-check run) the stub does what its name says.
        check_rejects = write_stub(
            work / "check_rejects.sh",
            'if [ "$LOXPP_CHECK_ONLY" = "1" ]; then\n'
            '    echo "stub: check rejects" >&2\n    exit 1\nfi\nexit 1\n',
        )
        check_accepts = write_stub(
            work / "check_accepts.sh",
            'if [ "$LOXPP_CHECK_ONLY" = "1" ]; then exit 0; fi\nexit 1\n',
        )
        check_hangs = write_stub(
            work / "check_hangs.sh",
            'if [ "$LOXPP_CHECK_ONLY" = "1" ]; then sleep 30; fi\nexit 1\n',
        )
        prints_partial = write_stub(
            work / "prints_partial.sh", 'echo "partial"\nexit 1\n'
        )

        cases = [
            ("1 native rejects, stub runs", str(acceptor), [static_error], 1, "compile phase"),
            ("2 native runs, stub rejects", str(rejector), [runs], 1, "DIVERGE"),
            ("3 native halts after CHECK lines, stub rejects", str(rejector), [halt_probe], 1,
             "compile phase"),
            ("4 run-time difference in stdout", str(extra_line), [runs], 1, "DIVERGE"),
            ("5 run-time difference in exit status", str(other_status), [runs], 1,
             "exit status disagrees"),
            ("6 halt with one more deferred line", str(halt_with_defer), [halt_probe], 0,
             "MATCH       "),
            ("7 control: native against itself", native, [static_error, runs, halt_probe], 0,
             "3 matched"),
        ]
        check_cases = [
            ("8 non-probe halt, --other-check rejects", check_rejects, [halt_plain],
             ("--other-check", check_rejects), 1, "compile phase"),
            ("9 non-probe halt, --other-check accepts", check_accepts, [halt_plain],
             ("--other-check", check_accepts), 0, "MATCH       "),
            ("10 non-probe halt, --other-check does not finish", check_hangs, [halt_plain],
             ("--other-check", check_hangs, "--timeout", "2"), 1, "did not finish"),
            # Without the flag the check never runs, so a silent halt matches.
            ("11 non-probe halt, no --other-check", check_rejects, [halt_plain],
             (), 0, "MATCH       "),
            # The other runner printed output, so it compiled the program: the
            # check must not run, even though it would reject.
            ("12 non-probe halt, --other-check, other prints output", prints_partial,
             [halt_plain], ("--other-check", check_rejects), 0, "MATCH       "),
        ]
        rows = [(name, runner, programs, (), want_code, want_text)
                for name, runner, programs, want_code, want_text in cases]
        rows += check_cases
        failures = []
        for name, runner, programs, extra, want_code, want_text in rows:
            code, out = diff(native, runner, programs, extra)
            if code != want_code or want_text not in out:
                failures.append(
                    f"case {name}: want exit {want_code} and {want_text!r}, got exit {code}:\n"
                    + "\n".join("    " + line for line in out.splitlines())
                )
            else:
                print(f"ok    case {name}")

    if failures:
        print()
        for failure in failures:
            print(f"FAIL  {failure}")
        return 1
    print(f"OK: {len(rows)} case(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
