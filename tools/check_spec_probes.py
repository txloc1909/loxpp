#!/usr/bin/env python3
"""Runs the spec probes of a directory on one consumer, with that consumer's
expected-failure list.

A spec probe is a .lox file that carries one `// STATUS: ok` or
`// STATUS: error` line and one `// CHECK: <line>` directive per line of its
expected stdout, compared with trailing white space removed.

- `STATUS: ok`: the stdout lines are exactly the CHECK lines, and the exit
  status is zero.
- `STATUS: error`: the program halts (spec/README.md, Conformance). The exit
  status is non-zero, and the stdout lines start with the CHECK lines: the
  output before the fault binds, and the deferred calls of a halt may print
  more, because whether they run is implementation-defined.
- `STATUS: error` with `// STATIC-ERROR`: the program is rejected before it
  runs (spec/README.md, Conformance). The exit status is non-zero, and
  nothing prints: the stdout is empty. An implementation that finds the error
  only when it runs the program fails the probe if a line of the probe printed
  first, so a static-error probe has a `print` before the bad form. A
  static-error probe has no CHECK line.
- A program that ends through `exit(code)` does not halt. Its probe carries
  `// EXIT: <code>`: the exit status must be exactly that code, and the
  stdout lines are exactly the CHECK lines.

A probe that fails by hanging can carry `// TIMEOUT: <seconds>`, which takes
the place of --timeout for that probe; a run that times out fails. A .lox
file with no STATUS line is not a spec probe and is not run here.

A probe never checks stderr. Every report and message is
implementation-defined, so a `// STDERR:` line is an error (exit 2).

A probe whose CHECK lines depend on implementation-defined behavior that one
consumer chooses differently (spec/README.md, Conformance) carries
`// NOT-BINDING: <consumer> <probe>`, for example
`// NOT-BINDING: bootstrap bootstrap-only/one_thread_blocking_call.lox`. The
probe's header names that behavior. The second field is the spec probe, by
path from <dir>, that checks that consumer's own choice in its place: it
must exist and carry a STATUS line, and the consumer's own gate runs it. With
--consumer <consumer>, this script does not run the probe and reports it as
EXEMPT. Without --consumer, or for another consumer, the line changes nothing.
A NOT-BINDING probe is not a gap, so it must not also be on the
expected-failure list.

A probe passes when its stdout and its exit status agree.

The expected-failure list holds one line per probe that the consumer does not
pass yet: the file name, then a reason. Blank lines and lines that start with
'#' are comments. The lists are files under tools/spec-probe-xfail/, one for each
consumer, each passed with --xfail. The bootstrap interpreter
has no list: it uses NOT-BINDING lines. A consumer that has no gap needs no
file, and a node that finds a gap creates one. A file that does not exist in an existing directory is
no gap: it is reported as a NOTE and grades every probe. A path whose directory does not exist is a
misnamed path and an error (exit code 2). Outcomes:

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
STATIC_ERROR_RE = re.compile(r"^\s*//\s*STATIC-ERROR\s*$")


@dataclass
class SpecProbe:
    path: Path
    status: str  # "ok" or "error"
    checks: list[str]
    exit_code: int | None = None  # the exact exit status, when the probe names one
    timeout: float | None = None  # seconds, in place of --timeout
    # consumer -> the probe, by path from the probe directory, that holds the
    # consumer's own choice in place of this one
    not_binding: dict[str, str] | None = None
    # the program is rejected before it runs, so nothing prints
    static_error: bool = False


def parse_probe(path: Path) -> SpecProbe | None:
    """Returns the probe's expectations, or None if it has no STATUS line."""
    status = None
    exit_code = None
    timeout = None
    checks: list[str] = []
    not_binding: dict[str, str] = {}
    static_error = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if STATIC_ERROR_RE.match(line):
            static_error = True
            continue
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
        if STDERR_RE.match(line):
            raise ValueError(f"{path}: a STDERR line; the spec binds no stderr text")
    if status is None:
        return None
    if exit_code is not None and (exit_code == 0) != (status == "ok"):
        raise ValueError(f"{path}: EXIT {exit_code} disagrees with STATUS {status}")
    if static_error and (status != "error" or checks or exit_code is not None):
        raise ValueError(f"{path}: a STATIC-ERROR probe has `// STATUS: error`, no CHECK line, and no EXIT line")
    return SpecProbe(path, status, checks, exit_code, timeout, not_binding, static_error)


def stdout_lines(stdout: str) -> list[str]:
    return [line.rstrip() for line in stdout.splitlines()]


def halts(probe: SpecProbe) -> bool:
    """True for a probe of a halt: only the output before the fault binds."""
    return probe.status == "error" and probe.exit_code is None


def stdout_ok(probe: SpecProbe, stdout: str) -> bool:
    lines = stdout_lines(stdout)
    if probe.static_error:
        return lines == []
    if halts(probe):
        return lines[:len(probe.checks)] == probe.checks
    return lines == probe.checks


def exit_ok(probe: SpecProbe, exit_code: int) -> bool:
    if probe.exit_code is not None:
        return exit_code == probe.exit_code
    return (exit_code == 0) == (probe.status == "ok")


def passes(probe: SpecProbe, stdout: str, exit_code: int) -> bool:
    """True when one run's stdout and exit status match the probe."""
    return stdout_ok(probe, stdout) and exit_ok(probe, exit_code)


def parse_xfail_file(path: Path) -> dict[str, str]:
    """The entries of one expected-failure list.

    An absent list means the consumer passes every probe: no file exists while
    no gap is open (test/translation-probes/README.md). Print that, and return
    no entries, so deleting a list and dropping the flag that named it can land
    in either order. A list that exists is read strictly: an entry that names no
    probe of the run is still a failure.
    """
    if not path.parent.is_dir():
        # A missing list in a directory that exists is no gap, but a missing
        # directory is a misnamed path, and a NOTE would let it grade nothing.
        print(f"error: {path} cannot be a list: {path.parent} is not a directory", file=sys.stderr)
        sys.exit(2)
    if not path.exists():
        print(f"NOTE: {path} does not exist, so no probe is expected to fail")
        return {}
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
        try:
            probe = parse_probe(path)
        except ValueError as exc:
            print(f"error: {exc}", file=sys.stderr)
            sys.exit(2)
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
                  f"{exempt[name]} checks its own choice)")
            continue
        timeout = probe.timeout if probe.timeout is not None else args.timeout
        result = run(args.runner, probe, timeout)
        ok = result is not None and passes(probe, result[0], result[2])
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
            if not exit_ok(probe, code):
                if probe.exit_code is not None:
                    want = str(probe.exit_code)
                else:
                    want = "zero" if probe.status == "ok" else "non-zero"
                print(f"      exit code {code}, expected {want}")
            if not stdout_ok(probe, stdout):
                actual_stdout = stdout_lines(stdout)
                if probe.static_error:
                    print("      (a static error: the program is rejected, so nothing prints)")
                elif halts(probe):
                    actual_stdout = actual_stdout[:len(probe.checks)]
                    print("      (a halt: the stdout must start with the CHECK lines)")
                for line in difflib.unified_diff(
                    probe.checks, actual_stdout, "expected stdout", "actual stdout",
                    lineterm=""
                ):
                    print(f"      {line}")

    print()
    print(", ".join(f"{n} {k.lower()}" for k, n in counts.items()))
    sys.exit(1 if counts["FAIL"] or counts["XPASS"] else 0)


if __name__ == "__main__":
    main()
