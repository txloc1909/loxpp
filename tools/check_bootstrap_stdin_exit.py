#!/usr/bin/env python3
"""
Checks that a bootstrap wrapper exits when its interpreter exits, even while
the caller keeps stdin open.

The wrappers forward the caller's stdin to input() in the interpreted
program. When the program ends, that forwarder can still be blocked in a
read, and a wrapper that waits for it stays alive until stdin closes. This
check keeps the write end of a pipe open for the whole run, so only a
wrapper that exits on its own can pass.

Each case runs one small program through the wrapper and compares the exit
status and stdout:

  program-ends    print "hi";                     exit 0, "hi"
  compile-error   enum E { A, B }                 exit 65, error on stderr
  input           input(); print x;               exit 0, the line sent
  delayed-input   the same, sent after a delay    exit 0, the line sent

The input cases guard the other side of the change: the wrapper must not
stop the forwarder before the program has read what it needs. The messages
of a compile error are implementation-defined (spec/README.md,
"Conformance"), so only the status and a non-empty stderr are checked.

LANGUAGE is always set to LOXPP here: only that mode forwards stdin, so an
inherited LANGUAGE=LOX would make the check pass without testing anything.

Usage:
    python3 tools/check_bootstrap_stdin_exit.py <runner> [<runner>...]

<runner> is bootstrap/lox_wrapper.sh, tools/loxpp_jvm_bootstrap.sh,
tools/loxpp_qbe_bootstrap.sh, or an equivalent runner. Run from the
repository root. Exits 0 when every case passes, 1 otherwise, and 2 when a
<runner> does not exist or cannot run.
"""

import argparse
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass
from pathlib import Path

from runner_precondition import refuse_unrunnable, require_runner


@dataclass(frozen=True)
class Case:
    name: str
    source: str
    status: int
    stdout: str
    feed: str = ""
    delay: float = 0.0
    stderr_nonempty: bool = False


CASES = (
    Case("program-ends", 'print "hi";\n', 0, "hi\n"),
    Case("compile-error", 'enum E { A, B }\nprint "after";\n', 65, "", stderr_nonempty=True),
    Case("input", 'var x = input();\nprint x;\n', 0, "hello\n", feed="hello\n"),
    Case(
        "delayed-input",
        'var x = input();\nprint x;\n',
        0,
        "hello\n",
        feed="hello\n",
        delay=0.5,
    ),
)


def run_case(runner: str, case: Case, timeout: float) -> tuple[bool, str]:
    """Runs one case with stdin held open. Returns (passed, detail)."""
    with tempfile.TemporaryDirectory() as tmp:
        program = Path(tmp) / f"{case.name}.lox"
        program.write_text(case.source, encoding="utf-8")

        read_fd, write_fd = os.pipe()
        writer = None
        started = time.monotonic()
        try:
            try:
                proc = subprocess.Popen(
                    [runner, str(program)],
                    stdin=read_fd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    env=dict(os.environ, LANGUAGE="LOXPP"),
                    # A wrapper that hangs has a live child tree (the feeder, the
                    # interpreter, sed). It goes in its own process group so the
                    # timeout path can kill the whole tree, not only the wrapper.
                    start_new_session=True,
                )
            except OSError as exc:
                refuse_unrunnable(runner, exc)
            os.close(read_fd)
            read_fd = -1

            if case.feed:
                if case.delay > 0:

                    def feed_later() -> None:
                        time.sleep(case.delay)
                        try:
                            os.write(write_fd, case.feed.encode())
                        except OSError:
                            # The wrapper exited before it asked for input;
                            # the status and stdout checks report that.
                            pass

                    writer = threading.Thread(target=feed_later, daemon=True)
                    writer.start()
                else:
                    os.write(write_fd, case.feed.encode())

            try:
                stdout, stderr = proc.communicate(timeout=timeout)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.communicate()
                return False, f"still running after {timeout:.0f}s with stdin open"
            elapsed = time.monotonic() - started

            if writer is not None:
                writer.join()

            problems = []
            if proc.returncode != case.status:
                problems.append(f"exit {proc.returncode}, expected {case.status}")
            if stdout != case.stdout:
                problems.append(f"stdout {stdout!r}, expected {case.stdout!r}")
            if case.stderr_nonempty and not stderr.strip():
                problems.append("stderr is empty, expected a diagnostic")
            if problems:
                return False, "; ".join(problems)
            return True, f"{elapsed:.1f}s"
        finally:
            if read_fd >= 0:
                os.close(read_fd)
            os.close(write_fd)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check that bootstrap wrappers exit with stdin still open."
    )
    parser.add_argument("runners", nargs="+", help="wrapper scripts to check")
    parser.add_argument(
        "--timeout",
        type=float,
        default=60.0,
        help="seconds one case may run (default: 60)",
    )
    args = parser.parse_args()

    for runner in args.runners:
        require_runner(runner)

    failures = 0
    for runner in args.runners:
        for case in CASES:
            passed, detail = run_case(runner, case, args.timeout)
            if passed:
                print(f"PASS {runner} {case.name} ({detail})")
            else:
                print(f"FAIL {runner} {case.name}: {detail}", file=sys.stderr)
                failures += 1

    if failures:
        print(f"{failures} case(s) failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
