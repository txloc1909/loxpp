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
probe that fails by hanging can carry `// TIMEOUT: <seconds>`, which takes
the place of --timeout for that probe; a run that times out fails. A .lox
file with no STATUS line is not a spec probe and is not run here.

A probe that pins stderr text carries one `// STDERR: <line>` directive per
expected line. The STDERR lines must be the first lines of stderr, in order,
with trailing white space removed; any later stderr line is not checked, so a
report may append its own stack trace or the tasks that wait. A probe with no
STDERR line does not check stderr.

A probe whose CHECK lines a spec exception releases one consumer from carries
`// NOT-BINDING: <consumer> <probe>`, for example
`// NOT-BINDING: bootstrap bootstrap-only/one_thread_blocking_call.lox`. The
probe's header cites the exception. The second field is the spec probe, by
path from <dir>, that holds that consumer to the exception in its place: it
must exist and carry a STATUS line, and the consumer's own gate runs it. With
--consumer <consumer>, this script does not run the probe and reports it as
EXEMPT. Without --consumer, or for another consumer, the line changes nothing.
A NOT-BINDING probe is not a gap, so it must not also be on the
expected-failure list.

A probe passes when its stdout, its stderr prefix, and its exit status agree.

The expected-failure list holds one line per probe that the consumer does not
pass yet: the file name, then a reason. Blank lines and lines that start with
'#' are comments. Outcomes:

  PASS   not on the list, and the probe passes.
  FAIL   not on the list, and the probe fails. Exit code 1.
  XFAIL  on the list, and the probe fails.
  XPASS  on the list, and the probe passes. Exit code 1: the consumer now
         passes it, so delete its line from the list.
  EXEMPT a NOT-BINDING line names the --consumer. Not run.

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
STDERR_RE = re.compile(r"^\s*//\s*STDERR:\s?(.*)$")
EXIT_RE = re.compile(r"^\s*//\s*EXIT:\s*(-?\d+)\s*$")
TIMEOUT_RE = re.compile(r"^\s*//\s*TIMEOUT:\s*(\d+(?:\.\d+)?)\s*$")
NOT_BINDING_RE = re.compile(r"^\s*//\s*NOT-BINDING:\s*(\S+)\s+(\S+)\s*$")


@dataclass
class SpecProbe:
    path: Path
    status: str  # "ok" or "error"
    checks: list[str]
    stderr_checks: list[str]  # the expected first lines of stderr
    exit_code: int | None = None  # the exact exit status, when the probe names one
    timeout: float | None = None  # seconds, in place of --timeout
    # consumer -> the probe, by path from the probe directory, that holds the
    # consumer to the spec exception in place of this one
    not_binding: dict[str, str] | None = None


def parse_probe(path: Path) -> SpecProbe | None:
    """Returns the probe's expectations, or None if it has no STATUS line."""
    status = None
    exit_code = None
    timeout = None
    checks: list[str] = []
    stderr_checks: list[str] = []
    not_binding: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        m = NOT_BINDING_RE.match(line)
        if m:
            if m.group(1) in not_binding:
                raise ValueError(f"{path}: more than one NOT-BINDING line for {m.group(1)}")
            not_binding[m.group(1)] = m.group(2)
            continue
        m = TIMEOUT_RE.match(line)
        if m:
            timeout = float(m.group(1))
            continue
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
            continue
        m = STDERR_RE.match(line)
        if m:
            stderr_checks.append(m.group(1).rstrip())
    if status is None:
        return None
    if exit_code is not None and (exit_code == 0) != (status == "ok"):
        raise ValueError(f"{path}: EXIT {exit_code} disagrees with STATUS {status}")
    return SpecProbe(path, status, checks, stderr_checks, exit_code, timeout,
                     not_binding)


def stdout_lines(stdout: str) -> list[str]:
    return [line.rstrip() for line in stdout.splitlines()]


def stderr_lines(stderr: str) -> list[str]:
    return [line.rstrip() for line in stderr.splitlines()]


def passes(probe: SpecProbe, stdout: str, stderr: str, exit_code: int) -> bool:
    """True when one run's stdout, stderr prefix, and exit status match the probe."""
    if stdout_lines(stdout) != probe.checks:
        return False
    if (probe.stderr_checks and
            stderr_lines(stderr)[:len(probe.stderr_checks)] != probe.stderr_checks):
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


def run(runner: str, probe: SpecProbe, timeout: float) -> tuple[str, str, int] | None:
    """Runs one probe. Returns (stdout, stderr, exit code), or None on a timeout."""
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
    return result.stdout, result.stderr, result.returncode


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("runner")
    parser.add_argument("dir", type=Path)
    parser.add_argument("--xfail", type=Path, default=None)
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--consumer", default=None,
                        help="the consumer the runner is, for NOT-BINDING lines")
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

    exempt: dict[str, str] = {}
    for probe in probes:
        for consumer, replacement in probe.not_binding.items():
            target = args.dir / replacement
            replacement_probe = parse_probe(target) if target.is_file() else None
            if replacement_probe is None:
                print(f"error: {probe.path} names {replacement} on its NOT-BINDING line "
                      f"for {consumer}, which is not a spec probe in {args.dir}", file=sys.stderr)
                sys.exit(2)
            if consumer == args.consumer:
                exempt[probe.path.name] = replacement
    both = sorted(set(exempt) & set(xfail))
    if both:
        for name in both:
            print(f"error: {name} is NOT-BINDING for {args.consumer} and also on "
                  f"{args.xfail}; a probe the spec does not bind is not a gap", file=sys.stderr)
        sys.exit(2)

    counts = {"PASS": 0, "FAIL": 0, "XFAIL": 0, "XPASS": 0, "EXEMPT": 0}
    for probe in probes:
        name = probe.path.name
        if name in exempt:
            counts["EXEMPT"] += 1
            print(f"EXEMPT {name}  (does not bind {args.consumer}; "
                  f"{exempt[name]} holds it to the exception)")
            continue
        timeout = probe.timeout if probe.timeout is not None else args.timeout
        result = run(args.runner, probe, timeout)
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
                print(f"      timeout after {timeout:.0f}s")
                continue
            stdout, stderr, code = result
            if probe.exit_code is not None:
                want = str(probe.exit_code)
                exit_ok = code == probe.exit_code
            else:
                want = "zero" if probe.status == "ok" else "non-zero"
                exit_ok = (code == 0) == (probe.status == "ok")
            if not exit_ok:
                print(f"      exit code {code}, expected {want}")
            actual_stdout = stdout_lines(stdout)
            if actual_stdout != probe.checks:
                for line in difflib.unified_diff(
                    probe.checks, actual_stdout, "expected stdout", "actual stdout",
                    lineterm=""
                ):
                    print(f"      {line}")
            actual_stderr = stderr_lines(stderr)
            if (probe.stderr_checks and
                    actual_stderr[:len(probe.stderr_checks)] != probe.stderr_checks):
                for line in difflib.unified_diff(
                    probe.stderr_checks, actual_stderr, "expected stderr prefix",
                    "actual stderr", lineterm=""
                ):
                    print(f"      {line}")

    print()
    print(", ".join(f"{n} {k.lower()}" for k, n in counts.items()))
    sys.exit(1 if counts["FAIL"] or counts["XPASS"] else 0)


if __name__ == "__main__":
    main()
