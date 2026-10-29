#!/usr/bin/env python3
"""Checks that the print lines of two workers never mix inside one line.

spec/04-semantics.md, Workers and Tasks, Ending: "A `print` statement writes
its whole line at once: lines that tasks on different workers print never
mix inside one line." The order of the lines of two workers is not defined,
so an exact-stdout probe cannot pin this rule. This check runs
test/print-lines/two_workers.lox on one consumer and does not use the order
of lines between workers. It fails when:

  - the run does not exit 0, or does not finish in --timeout seconds;
  - a line does not have the form "w<id> <index> <pad>", where <id> is 1 or
    2, <index> has four digits, and <pad> is 64 copies of the letter of that
    worker ("a" for 1, "b" for 2);
  - the lines of one worker do not have the indexes 0000 to 0999, each once,
    in order.

A line that mixes the text of two workers has the wrong form, so the check
fails on it. The bootstrap interpreter runs every worker on one host thread
(spec/README.md, Conformance: scheduling is implementation-defined), so its lines cannot
mix, but the rule binds it too, and the check runs on it the same way.

Usage:
    tools/check_print_lines.py <runner> [--program PATH] [--timeout SECONDS]

<runner> is build/loxpp, bootstrap/lox_wrapper.sh (with LANGUAGE=LOXPP),
tools/loxpp_jvm.sh, tools/loxpp_qbe.sh, or an equivalent command that takes
the program path as its one argument.

Exit status: 0 when the check passes, 1 when it fails, 2 when <runner> does not
exist or cannot run.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

from runner_precondition import refuse_unrunnable, require_runner

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_PROGRAM = REPO_ROOT / "test" / "print-lines" / "two_workers.lox"

# These match test/print-lines/two_workers.lox.
LINES_PER_WORKER = 1000
PAD_LENGTH = 64
LETTERS = {"1": "a", "2": "b"}
LINE_RE = re.compile(r"^w([12]) (\d{4}) ([ab]+)$")


def check(stdout: str) -> list[str]:
    """Returns one problem per line or per worker that breaks the rule."""
    problems: list[str] = []
    indexes: dict[str, list[int]] = {wid: [] for wid in LETTERS}
    for number, line in enumerate(stdout.splitlines(), start=1):
        m = LINE_RE.match(line)
        if m is None or m.group(3) != LETTERS[m.group(1)] * PAD_LENGTH:
            problems.append(f"line {number} does not have the form of one worker: {line!r}")
            continue
        indexes[m.group(1)].append(int(m.group(2)))
    expected = list(range(LINES_PER_WORKER))
    for wid, got in indexes.items():
        if got != expected:
            missing = sorted(set(expected) - set(got))
            problems.append(
                f"worker {wid}: {len(got)} lines, expected {LINES_PER_WORKER} with the "
                f"indexes 0000 to {LINES_PER_WORKER - 1:04d} in order"
                + (f" (first missing index {missing[0]:04d})" if missing else "")
            )
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("runner")
    parser.add_argument("--program", type=Path, default=DEFAULT_PROGRAM)
    parser.add_argument("--timeout", type=float, default=300.0)
    args = parser.parse_args()

    require_runner(args.runner)

    try:
        result = subprocess.run(
            [args.runner, str(args.program)],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            errors="replace",
            timeout=args.timeout,
        )
    except subprocess.TimeoutExpired:
        print(f"FAIL  {args.program}: no end in {args.timeout:.0f}s")
        return 1
    except OSError as exc:
        refuse_unrunnable(args.runner, exc)

    problems = check(result.stdout)
    if result.returncode != 0:
        problems.insert(0, f"exit code {result.returncode}, expected 0")
    if problems:
        print(f"FAIL  {args.program}: {len(problems)} problem(s)")
        for problem in problems[:20]:
            print(f"      {problem}")
        if len(problems) > 20:
            print(f"      ... ({len(problems) - 20} more)")
        return 1
    count = LINES_PER_WORKER * len(LETTERS)
    print(f"PASS  {args.program}: {count} lines, none mixed, each worker in order")
    return 0


if __name__ == "__main__":
    sys.exit(main())
