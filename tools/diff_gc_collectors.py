#!/usr/bin/env python3
"""Compare the output of the collectors on the example corpus.

Usage: diff_gc_collectors.py <loxpp-binary> <examples-dir> [--stress]
                             [--exclude <file>]... [--timeout <seconds>]
       diff_gc_collectors.py --self-test <loxpp-binary>

Runs each example that tools/check_examples.py runs (a file with a CHECK
directive, not excluded, with its .input file as stdin) with LOXPP_GC=stw and
with LOXPP_GC=incremental. The stdout, the stderr and the exit status must be
equal. Only the examples are compared: the translation probes have expected
output that the native checks already hold under each collector, and the QBE
step compares them with the native VM. Every
program gives the same output with each collector (decision, goal 4), so a
difference is a defect of one collector. --stress adds LOXPP_STRESS_GC=1 to
the incremental run.

The corpus count is checked: the programs compared plus the programs excluded
must equal the programs that have a CHECK directive, and none may be skipped
for another reason. The run prints both counts.

Exit status: 0 pass, 1 a difference or a count that does not fit, 2 usage
error or a runner that cannot run.
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

from check_examples import extract_checks, extract_timeout, parse_exclude_file
from runner_precondition import refuse_unrunnable, require_runner

STRIP = ("LOXPP_GC", "LOXPP_STRESS_GC", "LOXPP_GC_VERIFY", "LOXPP_GC_TRACE")


def run(runner, prog, extra, timeout):
    env = {k: v for k, v in os.environ.items() if k not in STRIP}
    env.update(extra)
    inp = prog.with_suffix(".input")
    stdin = open(inp, "rb") if inp.exists() else subprocess.DEVNULL
    try:
        r = subprocess.run([runner, str(prog)], env=env, stdin=stdin,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=timeout)
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return "timeout", b"", b""
    except OSError as exc:
        refuse_unrunnable(runner, exc)
    finally:
        if inp.exists():
            stdin.close()


def compare(binary, examples, excluded, stress, timeout, incremental_runner=None):
    files = sorted(examples.glob("*.lox"))
    if not files:
        print(f"error: no .lox files found: {examples}", file=sys.stderr)
        sys.exit(2)
    eligible = [f for f in files if extract_checks(f)]
    compared = []
    skipped = []
    bad = []
    for f in eligible:
        if f.name in excluded:
            skipped.append(f.name)
            continue
        limit = extract_timeout(f) or timeout
        want = run(binary, f, {"LOXPP_GC": "stw"}, limit)
        extra = {"LOXPP_GC": "incremental"}
        if stress:
            extra["LOXPP_STRESS_GC"] = "1"
        got = run(incremental_runner or binary, f, extra, limit)
        compared.append(f.name)
        if want[0] == "timeout" or got[0] == "timeout":
            bad.append(f"{f.name}: a run ended by the time limit "
                       f"(stw {want[0]}, incremental {got[0]})")
        elif want != got:
            bad.append(f"{f.name}: stw exit {want[0]}, incremental exit "
                       f"{got[0]}; stdout "
                       f"{'differs' if want[1] != got[1] else 'is equal'}, "
                       f"stderr {'differs' if want[2] != got[2] else 'is equal'}")
    print(f"{len(eligible)} examples have a CHECK directive; compared "
          f"{len(compared)}, excluded {len(skipped)}")
    if len(compared) + len(skipped) != len(eligible) or not compared:
        bad.append("the counts do not fit")
    return bad


def self_test(binary):
    require_runner(binary)
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        (d / "ex").mkdir()
        (d / "ex" / "a.lox").write_text("// CHECK: 6\nprint 1 + 2 + 3;\n")
        (d / "ex" / "b.lox").write_text(
            "// CHECK: 7\nvar l = [];\nvar i = 0;\n"
            "while (i < 7) { l.append(i); i = i + 1; }\nprint len(l);\n")
        (d / "empty").mkdir()
        # A runner that behaves as the binary does, and one that prints one
        # more line when it runs the incremental collector.
        same = d / "same.sh"
        same.write_text(f'#!/bin/sh\nexec "{binary}" "$@"\n')
        other = d / "other.sh"
        other.write_text(
            f'#!/bin/sh\n"{binary}" "$@"; s=$?\n'
            'if [ "$LOXPP_GC" = incremental ]; then echo extra; fi\nexit $s\n')
        noisy = d / "noisy.sh"
        noisy.write_text(
            f'#!/bin/sh\n"{binary}" "$@"; s=$?\n'
            'if [ "$LOXPP_GC" = incremental ]; then echo fault >&2; fi\nexit $s\n')
        for p in (same, other, noisy):
            p.chmod(0o755)
        ex = d / "ex"
        if compare(str(same), ex, {}, False, 60):
            print("FAIL: two equal runs differ")
            sys.exit(1)
        if not compare(str(same), ex, {}, False, 60, str(other)):
            print("FAIL: a changed output did not fail the comparison")
            sys.exit(1)
        if not compare(str(same), ex, {}, False, 60, str(noisy)):
            print("FAIL: a difference on stderr did not fail the comparison")
            sys.exit(1)
        bad = compare(str(same), ex, {"a.lox", "b.lox"}, False, 60)
        if not bad:
            print("FAIL: a corpus with nothing compared passed")
            sys.exit(1)
        (d / "ex" / "c.lox").write_text("print 1;\n")
        if compare(str(same), ex, {}, False, 60):
            print("FAIL: an example with no CHECK changed the count")
            sys.exit(1)
        r = subprocess.run([sys.executable, __file__, str(same), str(d / "empty")],
                           capture_output=True)
        if r.returncode != 2:
            print("FAIL: an empty corpus did not exit 2")
            sys.exit(1)
    print("OK")


def main():
    args = sys.argv[1:]
    if args[:1] == ["--self-test"] and len(args) == 2:
        self_test(args[1])
        return
    stress = "--stress" in args
    if stress:
        args.remove("--stress")
    excluded = {}
    timeout = 60.0
    rest = []
    it = iter(args)
    for a in it:
        if a == "--exclude":
            excluded.update(parse_exclude_file(Path(next(it))))
        elif a == "--timeout":
            timeout = float(next(it))
        else:
            rest.append(a)
    if len(rest) != 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    binary = rest[0]
    require_runner(binary)
    bad = compare(binary, Path(rest[1]), excluded, stress, timeout)
    for b in bad:
        print(f"FAIL: {b}")
    if bad:
        sys.exit(1)
    print("OK")


if __name__ == "__main__":
    main()
