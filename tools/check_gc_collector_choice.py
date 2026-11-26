#!/usr/bin/env python3
"""Check LOXPP_GC, the choice of the collector when a heap starts.

Usage: check_gc_collector_choice.py <loxpp-binary>

An unset or empty variable and the name "stw" (and "incremental") must run a
program with the same output. An unknown name must print a message that names the variable and the
known collectors, run nothing, and end with status 64.

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


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


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
        for label, extra, unset in (
            ("unset", {}, ("LOXPP_GC",)),
            ("empty", {"LOXPP_GC": ""}, ()),
            ("stw", {"LOXPP_GC": "stw"}, ()),
            ("incremental", {"LOXPP_GC": "incremental"}, ()),
        ):
            r = run(binary, d, extra, unset)
            # A build that traces execution prints its trace around the output
            # of the program, so the check looks for the line of the program.
            if r.returncode != 0 or "ran" not in r.stdout.splitlines():
                fail(f"LOXPP_GC {label}: exit {r.returncode}, "
                     f"stdout {r.stdout!r}, stderr {r.stderr[:200]!r}")
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
