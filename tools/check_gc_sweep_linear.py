#!/usr/bin/env python3
"""Check that the collector's sweep phase is linear in the heap size.

Usage: check_gc_sweep_linear.py <loxpp-binary>

Keeps 200000 objects live, then allocates 300000 short-lived ones, so every
collection frees many objects out of a large heap. The sweep time comes from a
LOXPP_GC_TRACE file. A sweep that shifts the object table once per freed
object takes seconds on this program; a linear sweep takes tens of
milliseconds. The limit sits between the two with a wide margin on both sides.

Exit codes: 0 pass, 1 fail, 125 skipped (a build that traces execution prints
too much to run this program quickly).
"""

import os
import subprocess
import sys
import tempfile

SWEEP_LIMIT_S = 1.0

PROGRAM = """
var live = [];
var i = 0;
while (i < 200000) { live.append([i]); i = i + 1; }
i = 0;
while (i < 300000) { var t = [i]; i = i + 1; }
print len(live);
"""


def sweep_seconds(trace_path):
    total_ns = 0
    collections = 0
    with open(trace_path) as f:
        for line in f:
            if not line.startswith("gc "):
                continue
            fields = dict(p.split("=", 1) for p in line.split()[1:])
            total_ns += int(fields["t4"]) - int(fields["t3"])
            collections += 1
    return total_ns / 1e9, collections


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    if os.environ.get("LOXPP_EXEC_TRACE_ENABLED") == "1":
        print("skipped: build traces execution")
        return 125
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "prog.lox")
        trace = os.path.join(d, "gc.trace")
        with open(src, "w") as f:
            f.write(PROGRAM)
        env = {k: v for k, v in os.environ.items() if k != "LOXPP_STRESS_GC"}
        env["LOXPP_GC_TRACE"] = trace
        r = subprocess.run([binary, src], env=env, capture_output=True,
                           text=True, timeout=120)
        if r.returncode != 0:
            print(f"FAIL: exit {r.returncode}: {r.stderr[:300]}")
            return 1
        secs, n = sweep_seconds(trace)
    print(f"{n} collections, sweep total {secs:.3f} s (limit {SWEEP_LIMIT_S} s)")
    if n == 0:
        print("FAIL: no collection ran")
        return 1
    if secs > SWEEP_LIMIT_S:
        print("FAIL: sweep is not linear in the heap size")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
