#!/usr/bin/env python3
"""Check LOXPP_GC, the choice of the collector when a heap starts.

Usage: check_gc_collector_choice.py <loxpp-binary>

An unset or empty variable, "stw" and "incremental" must each run a program
with the same output, and the GC trace must show the collector that ran: a
`gc` record for "stw", a `cycle` record and no `gc` record for "incremental".
An unset or empty variable must run DEFAULT, the default collector that
section 10.1 of docs/decisions/garbage-collectors.md chooses. An unknown name
must print a message that names the variable and the known collectors, run
nothing, and end with status 64.

Exit status: 0 pass, 1 fail, 2 usage error or a binary that does not exist or
cannot run.
"""

import os
import subprocess
import sys
import tempfile

from runner_precondition import refuse_unrunnable, require_runner

TIMEOUT_S = 60
PROGRAM = 'print "ran";\n'
DEFAULT = "incremental"


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def collector_of(trace):
    """The collector that wrote a trace. Each heap collects once at exit, so
    even this short program writes one record."""
    kinds = set()
    with open(trace) as f:
        for line in f:
            if line.strip() and not line.startswith("#"):
                kinds.add(line.split()[0])
    if "gc" in kinds and "cycle" not in kinds:
        return "stw"
    if "cycle" in kinds and "gc" not in kinds:
        return "incremental"
    return f"unknown (records {sorted(kinds)})"


def run(binary, workdir, extra_env, unset=()):
    env = {k: v for k, v in os.environ.items() if k not in unset}
    env.update(extra_env)
    try:
        return subprocess.run(
            [binary, os.path.join(workdir, "prog.lox")],
            cwd=workdir,
            env=env,
            capture_output=True,
            text=True,
            timeout=TIMEOUT_S,
        )
    except subprocess.TimeoutExpired:
        fail(f"no end in {TIMEOUT_S} s")
    except OSError as exc:
        refuse_unrunnable(binary, exc)


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    require_runner(sys.argv[1])
    binary = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory() as d:
        with open(os.path.join(d, "prog.lox"), "w") as f:
            f.write(PROGRAM)
        trace = os.path.join(d, "gc.trace")
        for label, extra, unset, want in (
            ("unset", {}, ("LOXPP_GC",), DEFAULT),
            ("empty", {"LOXPP_GC": ""}, (), DEFAULT),
            ("stw", {"LOXPP_GC": "stw"}, (), "stw"),
            ("incremental", {"LOXPP_GC": "incremental"}, (), "incremental"),
        ):
            if os.path.exists(trace):
                os.unlink(trace)
            r = run(binary, d, {**extra, "LOXPP_GC_TRACE": trace}, unset)
            # A build that traces execution prints its trace around the output
            # of the program, so the check looks for the line of the program.
            if r.returncode != 0 or "ran" not in r.stdout.splitlines():
                fail(f"LOXPP_GC {label}: exit {r.returncode}, "
                     f"stdout {r.stdout!r}, stderr {r.stderr[:200]!r}")
            have = collector_of(trace)
            if have != want:
                fail(f"LOXPP_GC {label}: the trace is of {have}, not {want}")
        for bad in ("Incremental", "STW", "stw ", "nope"):
            r = run(binary, d, {"LOXPP_GC": bad})
            if r.returncode != 64:
                fail(f"LOXPP_GC={bad!r}: exit {r.returncode}, not 64")
            if r.stdout:
                fail(f"LOXPP_GC={bad!r}: the program ran: {r.stdout!r}")
            for needle in ("LOXPP_GC", f"unknown collector '{bad}'", "known: stw, incremental"):
                if needle not in r.stderr:
                    fail(f"LOXPP_GC={bad!r}: the message has no '{needle}': "
                         f"{r.stderr!r}")
    print("OK")


if __name__ == "__main__":
    main()
