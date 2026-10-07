#!/usr/bin/env python3
"""Check that a small live heap does not make the collector run too often.

Usage: check_gc_min_heap.py <loxpp-binary>

Keeps one small list live and allocates 300000 short-lived lists, about 20 MB
in total. With a next-collection threshold of twice the live heap and no
minimum, the threshold is a few kilobytes and the program collects about a
thousand times. With a minimum threshold of one megabyte it collects about
twenty times. The collection count comes from a LOXPP_GC_TRACE file. The limit
sits between the two with a wide margin on both sides.

Exit codes: 0 pass, 1 fail, 125 skipped (a build that traces execution prints
too much to run this program quickly).
"""

import os
import subprocess
import sys
import tempfile

COLLECTION_LIMIT = 100

PROGRAM = """
var keep = [1];
var i = 0;
while (i < 300000) { var t = [i, i]; i = i + 1; }
print len(keep);
"""


def count_collections(trace_path):
    with open(trace_path) as f:
        return sum(1 for line in f if line.startswith("gc "))


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
        n = count_collections(trace)
    print(f"{n} collections (limit {COLLECTION_LIMIT})")
    if n == 0:
        print("FAIL: no collection ran")
        return 1
    if n > COLLECTION_LIMIT:
        print("FAIL: a small live heap makes the collector run too often")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
