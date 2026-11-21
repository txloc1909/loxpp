#!/usr/bin/env python3
"""Check the large-heap programs of benchmarks/gc_large_heap/.

Usage: check_gc_large_heap.py <loxpp-binary>

Each program keeps a large live heap and makes garbage, so that a collector
step that grows with the heap shows in a GC trace. They are too big for a
test, so this check runs each one with smaller constants and compares its
output with the value that the constants give by arithmetic. It also reads the
GC trace, and requires that the program made collections: a program that
never collects measures nothing.

Exit status: 0 pass, 1 fail, 2 usage error or a binary that cannot run.
"""

import os
import re
import subprocess
import sys
import tempfile

from runner_precondition import refuse_unrunnable, require_runner

HERE = os.path.dirname(os.path.abspath(__file__))
PROGRAMS = os.path.join(HERE, "..", "benchmarks", "gc_large_heap")
TIMEOUT_S = 300
MIN_COLLECTIONS = 3


def tri(n):
    """The sum of 0 .. n-1."""
    return n * (n - 1) // 2


# program: (small constants, the value it must print)
CASES = {
    "live_then_churn": (
        {"LIVE": 60000, "CHURN": 120000},
        lambda c: tri(c["CHURN"]) + c["LIVE"],
    ),
    "list_write": (
        {"N": 60000, "ROUNDS": 80000},
        lambda c: c["N"],
    ),
    "big_map": (
        {"N": 40000, "ROUNDS": 60000},
        lambda c: c["N"],
    ),
    "long_chain": (
        {"N": 60000, "ROUNDS": 100000},
        lambda c: c["N"] + tri(c["ROUNDS"]),
    ),
}


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def scaled(source, consts, name):
    for key, value in consts.items():
        source, n = re.subn(
            rf"^var {key} = \d+;", f"var {key} = {value};", source, flags=re.M
        )
        if n != 1:
            fail(f"{name}: expected one 'var {key} = <number>;', found {n}")
    return source


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    require_runner(sys.argv[1])
    binary = os.path.abspath(sys.argv[1])
    on_disk = sorted(f[:-4] for f in os.listdir(PROGRAMS) if f.endswith(".lox"))
    if on_disk != sorted(CASES):
        fail(f"programs on disk {on_disk} differ from the cases {sorted(CASES)}")
    env = {k: v for k, v in os.environ.items() if k != "LOXPP_STRESS_GC"}
    with tempfile.TemporaryDirectory() as d:
        for name, (consts, want) in sorted(CASES.items()):
            with open(os.path.join(PROGRAMS, f"{name}.lox")) as f:
                source = scaled(f.read(), consts, name)
            prog = os.path.join(d, f"{name}.lox")
            trace = os.path.join(d, f"{name}.trace")
            with open(prog, "w") as f:
                f.write(source)
            try:
                r = subprocess.run(
                    [binary, prog],
                    env={**env, "LOXPP_GC_TRACE": trace},
                    cwd=d,
                    capture_output=True,
                    text=True,
                    timeout=TIMEOUT_S,
                )
            except subprocess.TimeoutExpired:
                fail(f"{name}: no end in {TIMEOUT_S} s")
            except OSError as exc:
                refuse_unrunnable(binary, exc)
            if r.returncode != 0:
                fail(f"{name}: exit {r.returncode}: {r.stderr[:200]}")
            expect = str(want(consts))
            if r.stdout.strip() != expect:
                fail(f"{name}: printed {r.stdout.strip()!r}, arithmetic gives {expect}")
            collections = sum(
                1 for line in open(trace) if line.startswith("gc cause=threshold")
            )
            if collections < MIN_COLLECTIONS:
                fail(f"{name}: {collections} collections, at least {MIN_COLLECTIONS} needed")
    print("OK")


if __name__ == "__main__":
    main()
