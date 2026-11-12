#!/usr/bin/env python3
"""Holds tools/check_spec_probes.py to its rule for a time limit.

The tool took a probe's `// TIMEOUT:` value only when it was a bare positive
number, and it gave --timeout no rule at all (issue #1207). Two shapes came of
that. A value the pattern did not match, such as `-1` or `5s`, was dropped
without a word, so the probe ran with --timeout's 60 seconds and the mistake
read as a probe that hangs. A value the pattern did match and no syscall can
hold, such as `99999999999`, reached subprocess.run and came back as an
OverflowError from select(2), so the mistake read as a failed probe with a
traceback. tools/check_examples.py already answers both with one rule
(issue #1198); this holds the sibling tool to the same rule.

One rule decides both spellings: a finite number of seconds greater than zero
and at most a day. Any other value is a usage error: exit 2, with a message
that names the probe and the line, or the flag. A bad value must never read as
a probe that fails, because exit 1 means the consumer diverged from the spec.

Every case runs on a directory this file writes itself, so the corpus is not
walked and the whole run costs a few seconds. Each `check(...)` call is one
case and prints one `ok` line; no count is written here by hand, because a
hand-maintained count goes stale.

Usage:
    tools/check_spec_probes_limits.py --native <loxpp-binary>
"""

import argparse
import pathlib
import subprocess
import sys
import tempfile
import time

from runner_precondition import require_runner

ROOT = pathlib.Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "check_spec_probes.py"

# A spec probe that passes at once, so a case that must still run cannot owe
# its result to a limit.
PASSING = '// STATUS: ok\n// CHECK: done\nprint "done";\n'

# The same probe with a directive of its own, in place of PASSING's.
DIRECTIVE = '// STATUS: ok\n// TIMEOUT: {value}\n// CHECK: done\nprint "done";\n'

# A spec probe that does not end, written as a long sleep rather than a spin.
HANGING = '// STATUS: ok\n// TIMEOUT: {value}\n// CHECK: never\nsleep(60);\n'

# Every spelling the rule must refuse, and the one it must not. A day is the
# ceiling. "99999999999" is the overflow of issue #1207 and "5s" is a value the
# old pattern dropped without a word.
BAD_VALUES = ["-1", "0", "nan", "inf", "1e10", "86401", "99999999999", "5s", ""]

failures: list[str] = []


def not_refused(rc: int, out: str, took: float) -> bool:
    """True when the tool ran the probe instead of refusing a value.

    A build that traces each instruction prints its trace to stdout, so its
    probe fails the CHECK comparison and the tool exits 1. That is a run, and
    this file is about the values the tool refuses, so only exit 2 with a
    message counts as refused.
    """
    return rc != 2 and "error:" not in out and took < 10


def check(name: str, ok: bool, detail: str = "") -> None:
    if ok:
        print(f"ok   {name}")
    else:
        print(f"FAIL {name}  {detail}")
        failures.append(name)


def run(args: list[str], timeout: float = 60.0) -> tuple[int, str, float]:
    started = time.monotonic()
    proc = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    return proc.returncode, proc.stdout + proc.stderr, time.monotonic() - started


def write_dir(tmp: pathlib.Path, text: str) -> pathlib.Path:
    """One case directory, holding one probe, so no case answers for another."""
    d = tmp / f"case{len(list(tmp.iterdir()))}"
    d.mkdir()
    (d / "probe.lox").write_text(text, encoding="utf-8")
    return d


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    args = parser.parse_args()
    native = args.native
    require_runner(native)

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = pathlib.Path(tmpdir)

        # A runner that cannot run is a setup mistake (exit 2), and grading the
        # rest of the cases against it would read as that many failures.
        d = write_dir(tmp, PASSING)
        rc, out, took = run([sys.executable, str(TOOL), native, str(d)])
        if rc == 2:
            print(out, file=sys.stderr, end="")
            sys.exit(2)
        check("the runner runs one probe", not_refused(rc, out, took), out)

        # The rule itself, for the probe's own directive. The message must name
        # the probe and the line, so a run of 115 probes says which one to fix.
        for value in BAD_VALUES:
            d = write_dir(tmp, DIRECTIVE.format(value=value))
            rc, out, _ = run([sys.executable, str(TOOL), native, str(d)])
            check(
                f"// TIMEOUT: {value!r} is exit 2, not a failed probe",
                rc == 2 and "greater than zero" in out and "probe.lox:2:" in out,
                f"rc={rc} out={out[:300]!r}",
            )

        # The two shapes of issue #1207, which read as a failed probe before:
        # a dropped value ran for the flag's 60 seconds, and a value no syscall
        # can hold raised a traceback.
        d = write_dir(tmp, DIRECTIVE.format(value="-1"))
        rc, out, took = run(
            [sys.executable, str(TOOL), native, str(d), "--timeout", "1"]
        )
        check(
            "a dropped value is not answered by --timeout",
            rc == 2 and "timeout after" not in out and took < 10,
            f"rc={rc} took={took:.1f}s out={out[:300]!r}",
        )
        d = write_dir(tmp, DIRECTIVE.format(value="99999999999"))
        rc, out, _ = run([sys.executable, str(TOOL), native, str(d)])
        check(
            "a value too large for the syscall is exit 2, not a traceback",
            rc == 2 and "Traceback" not in out and "OverflowError" not in out,
            f"rc={rc} out={out[:300]!r}",
        )

        # The ceiling and the floor still run, so the rule refuses only what it
        # names.
        for value in ("86400", "1", "0.5", "  30  "):
            d = write_dir(tmp, DIRECTIVE.format(value=value))
            rc, out, took = run([sys.executable, str(TOOL), native, str(d)])
            check(
                f"// TIMEOUT: {value!r} still runs",
                not_refused(rc, out, took),
                f"rc={rc} took={took:.1f}s out={out[:300]!r}",
            )

        # A probe's own value still takes the place of --timeout, and a probe
        # with no directive still times out at the flag: the rule changed the
        # two limits it is about and nothing else.
        d = write_dir(tmp, HANGING.format(value="1"))
        rc, out, took = run(
            [sys.executable, str(TOOL), native, str(d), "--timeout", "60"]
        )
        check(
            "a probe's own value takes the place of --timeout",
            rc == 1 and "timeout after 1s" in out and took < 20,
            f"rc={rc} took={took:.1f}s out={out[:300]!r}",
        )
        d = write_dir(tmp, '// STATUS: ok\n// CHECK: never\nsleep(60);\n')
        rc, out, took = run(
            [sys.executable, str(TOOL), native, str(d), "--timeout", "1"]
        )
        check(
            "a probe with no directive still times out at --timeout",
            rc == 1 and "timeout after 1s" in out and took < 20,
            f"rc={rc} took={took:.1f}s out={out[:300]!r}",
        )

        # The same rule for the flag. A fresh directory, because the one above
        # carries a directive that would answer for the flag and leave its own
        # rule untested.
        d = write_dir(tmp, PASSING)
        for value in BAD_VALUES:
            rc, out, _ = run(
                [sys.executable, str(TOOL), native, str(d), f"--timeout={value}"]
            )
            check(
                f"--timeout {value!r} is exit 2, not a traceback",
                rc == 2 and "--timeout" in out and "Traceback" not in out,
                f"rc={rc} out={out[:300]!r}",
            )
        for value in ("5", "86400"):
            rc, out, took = run(
                [sys.executable, str(TOOL), native, str(d), "--timeout", value]
            )
            check(
                f"--timeout {value} still runs",
                not_refused(rc, out, took),
                f"rc={rc} took={took:.1f}s out={out[:300]!r}",
            )

    if failures:
        print(f"\n{len(failures)} case(s) failed")
        sys.exit(1)
    print("\nall cases passed")


if __name__ == "__main__":
    main()
