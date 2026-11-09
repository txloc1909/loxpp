#!/usr/bin/env python3
"""
Lightweight FileCheck-style tester for Lox++ example programs.

For each examples/*.lox file (alphabetical order):
  1. Extract all '// CHECK: <text>' directives in file order.
  2. If no CHECK directives are found, skip the file.
  3. If examples/<name>.input exists, pipe it as stdin; otherwise stdin is
     empty, so a run does not depend on what the caller's stdin holds.
  4. Run loxpp with a limit on its time, and match the CHECK directives as
     the output arrives, so neither the time nor the memory grows with what
     the program prints.
  5. For each CHECK directive, scan forward through the arriving output lines
     until a stripped line matches the CHECK text exactly.
  6. Report PASS / FAIL / TIMEOUT / SKIP per file, then exit 1 if any failed.

An example can carry `// TIMEOUT: <seconds>` to take the place of --timeout,
as the probes of test/translation-probes/ can for tools/check_spec_probes.py.

The capture is bounded on purpose. A build with LOXPP_DEBUG_TRACE_EXECUTION
prints one line for every instruction, so a single example can emit hundreds of
megabytes: `sieve.lox` emits 370 MB in 15 s on a release host and more on a
debug one. Reading that into a Python string and splitting it into lines is
what took a 7 GB python3 down with the kernel OOM killer (issue #1176). So the
run keeps only the lines it needs for the failure report, and an example whose
output passes --max-output-bytes fails by its own limit, named, instead of
exhausting the host.

Usage:
    python3 tools/check_examples.py <loxpp-binary> <examples-dir>
        [--exclude <file>]... [--timeout <seconds>] [--max-output-bytes <n>]

--exclude <file> names a file with one excluded example per line: the file
name, then a reason. Repeatable: pass --exclude more than once to skip names
from more than one list at once (for example, the current backend's
map-order list plus its own known-crash list — a name on either list is
skipped). tools/jvm_excluded_examples.txt is one such file — the
map-order-sensitive examples the JVM backend legitimately reorders (spec
leaves map iteration order unspecified). An excluded file is skipped, not
run, and reported as SKIP with its reason.

<examples-dir> that resolves to zero .lox files — because it does not
exist, or is empty — is an error (exit 2), not a silent zero-file pass:
Path.glob raises nothing on a missing directory, so a typo'd path would
otherwise report "0 passed, 0 failed, 0 skipped" and exit 0.

A <loxpp-binary> that does not exist or cannot run is also an error (exit 2): it
is a mistake in the setup, not a failed example.
"""

import os
import re
import select
import subprocess
import sys
import time
from pathlib import Path

from runner_precondition import refuse_unrunnable, require_runner


def parse_exclude_file(path: Path) -> dict[str, str]:
    """Reads a 'name  reason' exclusion list. Blank lines and lines starting
    with '#' are comments."""
    reasons: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        name, _, reason = stripped.partition(" ")
        reasons[name] = reason.strip()
    return reasons


def extract_checks(lox_file: Path) -> list[str]:
    checks = []
    for line in lox_file.read_text(encoding="utf-8").splitlines():
        m = re.match(r"\s*//\s*CHECK:\s*(.*)", line)
        if m:
            checks.append(m.group(1).rstrip())
    return checks


TIMEOUT_RE = re.compile(r"^\s*//\s*TIMEOUT:\s*(\d+(?:\.\d+)?)\s*$")

# Lines kept for the failure report: the head shows how the program started and
# the tail shows where it stopped. Neither grows with the output.
REPORT_HEAD_LINES = 20
REPORT_TAIL_LINES = 40


def extract_timeout(lox_file: Path) -> float | None:
    """The example's own `// TIMEOUT: <seconds>`, or None."""
    for line in lox_file.read_text(encoding="utf-8").splitlines():
        m = TIMEOUT_RE.match(line)
        if m:
            return float(m.group(1))
    return None


class Capture:
    """Matches CHECK lines as they arrive, with bounded memory.

    A line is matched against the next CHECK in order and then dropped, except
    for the first and last few, which the failure report prints.
    """

    def __init__(self, checks: list[str]):
        self.checks = checks
        self.cursor = 0
        self.head: list[str] = []
        self.tail: list[str] = []
        self.lines = 0
        self.bytes = 0

    def feed(self, line: str) -> None:
        self.lines += 1
        self.bytes += len(line) + 1
        if len(self.head) < REPORT_HEAD_LINES:
            self.head.append(line)
        else:
            self.tail.append(line)
            if len(self.tail) > REPORT_TAIL_LINES:
                del self.tail[0]
        if self.cursor < len(self.checks) and line.strip() == self.checks[self.cursor]:
            self.cursor += 1

    def missing(self) -> str | None:
        return None if self.cursor == len(self.checks) else self.checks[self.cursor]

    def report_lines(self) -> list[str]:
        if self.lines <= len(self.head) + len(self.tail):
            return list(self.head) + list(self.tail)
        return list(self.head) + ["..."] + list(self.tail)


def run_example(
    loxpp: str, lox_file: Path, input_file: Path, timeout: float, max_bytes: int
) -> tuple[str, Capture, int]:
    """Runs one example against its CHECK lines as its output arrives.

    Returns (status, capture, exit_code) with status one of
    "done", "timeout" or "output".
    """
    capture = Capture(extract_checks(lox_file))
    cmd = [loxpp, str(lox_file)]
    stdin: object = subprocess.DEVNULL
    if input_file.exists():
        stdin = open(input_file, "rb")
    # stderr is not matched against anything, and a debug build's GC log can be
    # gigabytes under LOXPP_STRESS_GC, so discard it rather than hold it.
    try:
        proc = subprocess.Popen(
            cmd,
            stdin=stdin,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
    except OSError as exc:
        refuse_unrunnable(loxpp, exc)
        raise
    finally:
        if input_file.exists():
            stdin.close()  # type: ignore[union-attr]

    deadline = time.monotonic() + timeout
    pending = b""
    status = "done"
    try:
        fd = proc.stdout.fileno()  # type: ignore[union-attr]
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                status = "timeout"
                break
            ready, _, _ = select.select([fd], [], [], remaining)
            if not ready:
                status = "timeout"
                break
            chunk = os.read(fd, 65536)
            if not chunk:
                break
            capture.bytes += len(chunk) - chunk.count(b"\n")
            if capture.bytes > max_bytes:
                status = "output"
                break
            pending += chunk
            # One split per chunk, not one per line: splitting the remainder at
            # each newline copies what is left, so a 64 KiB chunk of short lines
            # would copy megabytes of it.
            lines = pending.split(b"\n")
            pending = lines.pop()
            for raw in lines:
                capture.feed(raw.decode("utf-8", "replace"))
        if status == "done" and pending:
            capture.feed(pending.decode("utf-8", "replace"))
    finally:
        if status != "done" or proc.poll() is None:
            # A timed-out or over-size run is stopped, not left to finish in the
            # background; a finished one is reaped here either way.
            if status == "timeout" or status == "output":
                proc.kill()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    return status, capture, proc.returncode


USAGE = (
    f"Usage: {os.path.basename(sys.argv[0])} <loxpp-binary> <examples-dir> "
    "[--exclude <file>]... [--timeout <seconds>] [--max-output-bytes <n>]"
)


def take_option(args: list[str], name: str) -> str | None:
    """Removes `name <value>` from args and returns the value."""
    if name not in args:
        return None
    i = args.index(name)
    if i + 1 >= len(args):
        print(USAGE, file=sys.stderr)
        sys.exit(2)
    value = args[i + 1]
    del args[i : i + 2]
    return value


def main() -> None:
    args = sys.argv[1:]
    exclude_paths: list[Path] = []
    while "--exclude" in args:
        i = args.index("--exclude")
        if i + 1 >= len(args):
            print(USAGE, file=sys.stderr)
            sys.exit(2)
        exclude_paths.append(Path(args[i + 1]))
        del args[i : i + 2]

    timeout_arg = take_option(args, "--timeout")
    output_arg = take_option(args, "--max-output-bytes")
    try:
        timeout = float(timeout_arg) if timeout_arg is not None else 60.0
        max_bytes = int(output_arg) if output_arg is not None else 8 * 1024 * 1024
    except ValueError:
        print(USAGE, file=sys.stderr)
        sys.exit(2)
    if timeout <= 0 or max_bytes <= 0:
        print(USAGE, file=sys.stderr)
        sys.exit(2)

    if len(args) != 2:
        print(USAGE, file=sys.stderr)
        sys.exit(2)

    loxpp, examples_dir = args[0], Path(args[1])
    require_runner(loxpp)
    excluded: dict[str, str] = {}
    for exclude_path in exclude_paths:
        excluded.update(parse_exclude_file(exclude_path))

    lox_files = sorted(examples_dir.glob("*.lox"))
    if not lox_files:
        # Path.glob on a directory that does not exist raises nothing and
        # yields nothing, so a typo'd path would otherwise report "0 passed,
        # 0 failed, 0 skipped" and exit 0 — a check that ran against
        # nothing, indistinguishable from a check that passed everything.
        print(
            f"error: no .lox files found (not a directory, or empty): {examples_dir}",
            file=sys.stderr,
        )
        sys.exit(2)

    passed = failed = skipped = 0

    for lox_file in lox_files:
        if lox_file.name in excluded:
            print(f"SKIP  {lox_file.name}  (excluded: {excluded[lox_file.name]})")
            skipped += 1
            continue

        checks = extract_checks(lox_file)
        if not checks:
            print(f"SKIP  {lox_file.name}  (no CHECK directives)")
            skipped += 1
            continue

        input_file = lox_file.with_suffix(".input")
        own = extract_timeout(lox_file)
        limit = own if own is not None else timeout
        status, capture, rc = run_example(loxpp, lox_file, input_file, limit, max_bytes)

        if status == "timeout":
            # A run that hangs used to block the whole job until the workflow's
            # own limit, and the report named no example (#1176).
            print(f"TIMEOUT  {lox_file.name}  (killed after {limit:g}s)")
            failed += 1
            continue
        if status == "output":
            print(
                f"FAIL  {lox_file.name}  "
                f"(output over {max_bytes} bytes; a build that traces each "
                f"instruction prints that much)"
            )
            failed += 1
            continue
        if rc != 0:
            print(f"FAIL  {lox_file.name}  (exit code {rc})")
            failed += 1
            continue

        bad_check = capture.missing()
        if bad_check is None:
            print(f"PASS  {lox_file.name}  ({len(checks)} checks)")
            passed += 1
        else:
            print(f"FAIL  {lox_file.name}  (unmatched CHECK: '{bad_check}')")
            print("      actual output:")
            for line in capture.report_lines():
                print(f"        {line!r}")
            failed += 1

    print(f"\n{passed} passed, {failed} failed, {skipped} skipped")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
