#!/usr/bin/env python3
"""Check that the heap writes `stall` records and that the report reads them.

Usage: check_gc_stall.py <loxpp-binary>

The heap times each call that reaches the C allocator: a new chunk of spans
and a large block. A call that takes longer than the limit (100 us, or
LOXPP_GC_STALL_LIMIT_US) writes `stall kind=<new|free> t0= t1=` to the GC
trace. A small allocation or free never reaches the C allocator, so it
writes none.

Three runs of one program:
  1. With the limit at 0, every timed call writes a record. The trace must
     hold records of both kinds, each well formed, and tools/gc_report.py must
     show the longest stall of each kind and MMU with stalls.
  2. With the default limit, each record that exists must be at least 100 us
     long, and no `free` record may be longer than MAX_FREE_STALL_NS. A free
     of one large block in one call took 1.6 ms for 120 M elements before the
     heap gave memory back in slices.
  3. With a limit that no call reaches, the trace holds no stall record.

Exit status: 0 pass, 1 fail, 2 usage error or a binary that does not exist or
cannot run, 125 skipped (a build that traces execution prints too much to run
this program quickly).
"""

import os
import subprocess
import sys
import tempfile

from runner_precondition import refuse_unrunnable, require_runner

HERE = os.path.dirname(os.path.abspath(__file__))
TIMEOUT_S = 120
# The slice of a release is 1 MiB, which takes about 0.1 ms. One free of the
# whole 128 MB block takes about 5 ms. The limit sits between, with a margin
# on both sides, because the host stalls (decision section 13, item 6).
MAX_FREE_STALL_NS = 3_000_000

# A List that grows to a buffer of 128 MB, and then 300000 small Lists. The
# buffer is a large block: its growth frees old blocks, and the Lists need
# more chunks of spans. The free of the 128 MB block gives memory back in
# slices of 1 MiB, so no single free is long.
PROGRAM = """
var big = [];
var i = 0;
while (i < 16000000) { big.append(i); i = i + 1; }
big = nil;
var t = [];
var j = 0;
while (j < 300000) { t.append([j]); j = j + 1; }
print len(t);
"""


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def run(binary, workdir, name, limit_us):
    src = os.path.join(workdir, "prog.lox")
    with open(src, "w") as f:
        f.write(PROGRAM)
    trace = os.path.join(workdir, name + ".trace")
    env = {k: v for k, v in os.environ.items()
           if k not in ("LOXPP_GC_TRACE", "LOXPP_GC", "LOXPP_STRESS_GC",
                        "LOXPP_GC_STALL_LIMIT_US")}
    env["LOXPP_GC_TRACE"] = trace
    if limit_us is not None:
        env["LOXPP_GC_STALL_LIMIT_US"] = str(limit_us)
    try:
        r = subprocess.run([binary, src], cwd=workdir, env=env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                           text=True, timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        fail(f"{name}: the run did not end in {TIMEOUT_S} s")
    except OSError as exc:
        refuse_unrunnable(binary, exc)
    if r.returncode != 0:
        fail(f"{name}: the program exited with {r.returncode}: {r.stderr}")
    stalls = []
    with open(trace) as f:
        for n, line in enumerate(f, 1):
            if not line.startswith("stall "):
                continue
            kv = dict(p.split("=", 1) for p in line.split()[1:])
            if set(kv) != {"kind", "t0", "t1"}:
                fail(f"{name}: line {n}: bad fields: {line.strip()}")
            if kv["kind"] not in ("new", "free"):
                fail(f"{name}: line {n}: bad kind: {line.strip()}")
            stalls.append((kv["kind"], int(kv["t0"]), int(kv["t1"])))
    return trace, stalls


def report(trace):
    r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "gc_report.py"),
                        trace], capture_output=True, text=True, timeout=60)
    if r.returncode != 0:
        fail(f"gc_report.py failed: {r.stderr}")
    return r.stdout


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    binary = sys.argv[1]
    require_runner(binary)
    if os.environ.get("LOXPP_EXEC_TRACE_ENABLED") == "1":
        print("skipped: build traces execution")
        return 125
    with tempfile.TemporaryDirectory() as d:
        trace, stalls = run(binary, d, "all", 0)
        kinds = {k for k, _, _ in stalls}
        if kinds != {"new", "free"}:
            fail(f"limit 0: the kinds of the records are {sorted(kinds)}, "
                 "expected new and free")
        for kind, t0, t1 in stalls:
            if t1 < t0:
                fail(f"limit 0: a {kind} stall ends before it starts")
        out = report(trace)
        for needle in ("stall new", "stall free", "MMU with stalls"):
            if needle not in out:
                fail(f"gc_report.py does not show '{needle}':\n{out}")

        _, stalls = run(binary, d, "default", None)
        for kind, t0, t1 in stalls:
            if t1 - t0 < 100_000:
                fail(f"default limit: a {kind} stall of {t1 - t0} ns is "
                     "under the limit of 100 us")
            if kind == "free" and t1 - t0 > MAX_FREE_STALL_NS:
                fail(f"default limit: a free stall of {t1 - t0} ns is over "
                     f"{MAX_FREE_STALL_NS} ns")

        _, stalls = run(binary, d, "never", 100_000_000)
        if stalls:
            fail(f"a limit of 100 s still wrote {len(stalls)} records")
    print("pass: stall records written, read and limited")
    return 0


if __name__ == "__main__":
    sys.exit(main())
