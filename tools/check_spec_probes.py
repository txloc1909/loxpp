#!/usr/bin/env python3
"""Runs the spec probes of a directory on one consumer, with that consumer's
expected-failure list.

A spec probe is a .lox file that carries one `// STATUS: ok` or
`// STATUS: error` line and one `// CHECK: <line>` directive per line of its
expected stdout. The CHECK lines are the whole stdout the spec defines for
the program, in order: a probe passes only when the stdout lines, with
trailing white space removed, are exactly the CHECK lines, and when the exit
status is zero for `ok` and non-zero for `error`. A probe whose spec rule
defines the exact exit status, such as `exit(3)`, also carries
`// EXIT: <code>`, and then the exit status must be exactly that code. A
.lox file with no STATUS line is not a spec probe and is not run here.

The expected-failure list holds one line per probe that the consumer does not
pass yet: the file name, then a reason. Blank lines and lines that start with
'#' are comments. Outcomes:

  PASS   not on the list, and the probe passes.
  FAIL   not on the list, and the probe fails. Exit code 1.
  XFAIL  on the list, and the probe fails.
  XPASS  on the list, and the probe passes. Exit code 1: the consumer now
         passes it, so delete its line from the list.

A listed name with no spec probe behind it is an error (exit 2), so a stale
list cannot hide behind a rename.

If <name>.input exists next to a probe, it is piped in as stdin; otherwise
stdin is empty.

Usage:
    tools/check_spec_probes.py <runner> <dir> [--xfail FILE] [--timeout SECONDS]

<runner> is build/loxpp, bootstrap/lox_wrapper.sh (with LANGUAGE=LOXPP),
tools/loxpp_jvm.sh, tools/loxpp_qbe.sh, or an equivalent command that takes
the program path as its one argument.
"""

import argparse
import difflib
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

STATUS_RE = re.compile(r"^\s*//\s*STATUS:\s*(ok|error)\s*$")
CHECK_RE = re.compile(r"^\s*//\s*CHECK:\s?(.*)$")
EXIT_RE = re.compile(r"^\s*//\s*EXIT:\s*(-?\d+)\s*$")


@dataclass
class SpecProbe:
    path: Path
    status: str  # "ok" or "error"
    checks: list[str]
    exit_code: int | None = None  # the exact exit status, when the probe names one


def parse_probe(path: Path) -> SpecProbe | None:
    """Returns the probe's expectations, or None if it has no STATUS line."""
    status = None
    exit_code = None
    checks: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        m = EXIT_RE.match(line)
        if m:
            if exit_code is not None:
                raise ValueError(f"{path}: more than one EXIT line")
            exit_code = int(m.group(1))
            continue
        m = STATUS_RE.match(line)
        if m:
            if status is not None:
                raise ValueError(f"{path}: more than one STATUS line")
            status = m.group(1)
            continue
        m = CHECK_RE.match(line)
        if m:
            checks.append(m.group(1).rstrip())
    if status is None:
        return None
    if exit_code is not None and (exit_code == 0) != (status == "ok"):
        raise ValueError(f"{path}: EXIT {exit_code} disagrees with STATUS {status}")
    return SpecProbe(path, status, checks, exit_code)


def stdout_lines(stdout: str) -> list[str]:
    return [line.rstrip() for line in stdout.splitlines()]


def passes(probe: SpecProbe, stdout: str, exit_code: int) -> bool:
    """True when one run's stdout and exit status are what the probe says."""
    if stdout_lines(stdout) != probe.checks:
        return False
    if probe.exit_code is not None:
        return exit_code == probe.exit_code
    return (exit_code == 0) == (probe.status == "ok")


def parse_xfail_file(path: Path) -> dict[str, str]:
    reasons: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        name, _, reason = stripped.partition(" ")
        reasons[name] = reason.strip()
    return reasons


def run(runner: str, probe: SpecProbe, timeout: float) -> tuple[str, int] | None:
    """Runs one probe. Returns (stdout, exit code), or None on a timeout."""
    input_file = probe.path.with_suffix(".input")
    stdin_data = input_file.read_text(encoding="utf-8") if input_file.exists() else ""
    try:
        result = subprocess.run(
            [runner, str(probe.path)],
            input=stdin_data,
            capture_output=True,
            text=True,
            errors="replace",
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return None
    return result.stdout, result.returncode


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("runner")
    parser.add_argument("dir", type=Path)
    parser.add_argument("--xfail", type=Path, default=None)
    parser.add_argument("--timeout", type=float, default=60.0)
    args = parser.parse_args()

    if not args.dir.is_dir():
        print(f"error: not a directory: {args.dir}", file=sys.stderr)
        sys.exit(2)

    probes = []
    for path in sorted(args.dir.glob("*.lox")):
        probe = parse_probe(path)
        if probe is not None:
            probes.append(probe)
    if not probes:
        print(f"error: no spec probe (a .lox file with a STATUS line) in {args.dir}", file=sys.stderr)
        sys.exit(2)

    xfail = parse_xfail_file(args.xfail) if args.xfail else {}
    names = {p.path.name for p in probes}
    stale = sorted(set(xfail) - names)
    if stale:
        for name in stale:
            print(f"error: {args.xfail} lists {name}, which is not a spec probe in {args.dir}", file=sys.stderr)
        sys.exit(2)

    counts = {"PASS": 0, "FAIL": 0, "XFAIL": 0, "XPASS": 0}
    for probe in probes:
        name = probe.path.name
        result = run(args.runner, probe, args.timeout)
        ok = result is not None and passes(probe, *result)
        listed = name in xfail
        if listed:
            outcome = "XPASS" if ok else "XFAIL"
        else:
            outcome = "PASS" if ok else "FAIL"
        counts[outcome] += 1
        note = ""
        if outcome == "XFAIL":
            note = f"  ({xfail[name]})"
        elif outcome == "XPASS":
            note = f"  (passes now: delete its line from {args.xfail})"
        print(f"{outcome:<6}{name}{note}")
        if outcome == "FAIL":
            if result is None:
                print(f"      timeout after {args.timeout:.0f}s")
                continue
            stdout, code = result
            if probe.exit_code is not None:
                want = str(probe.exit_code)
            else:
                want = "zero" if probe.status == "ok" else "non-zero"
            print(f"      exit code {code}, expected {want}")
            for line in difflib.unified_diff(
                probe.checks, stdout_lines(stdout), "expected", "actual", lineterm=""
            ):
                print(f"      {line}")

    print()
    print(", ".join(f"{n} {k.lower()}" for k, n in counts.items()))
    sys.exit(1 if counts["FAIL"] or counts["XPASS"] else 0)


if __name__ == "__main__":
    main()
