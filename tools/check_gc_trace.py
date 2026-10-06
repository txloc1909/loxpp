#!/usr/bin/env python3
"""Check LOXPP_GC_TRACE: written when set, not written when unset.

Usage: check_gc_trace.py <loxpp-binary>

Runs the binary on a program that allocates enough to trigger collections,
then checks the trace file's shape and that tools/gc_report.py reads it.
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

# Doubling a string allocates about 8 MB in a few hundred instructions, enough
# to pass the first collection threshold. A debug build prints every executed
# instruction, so the instruction count must stay small.
PROGRAM = """
var s = "x";
for (var i = 0; i < 22; i = i + 1) {
    s = s + s;
}
"""

# Stress mode collects on every allocation, so this one must stay tiny.
STRESS_PROGRAM = 'var a = "a" + "b";'

FIELDS = (
    "cause t0 t1 t2 t3 t4 bytes_before bytes_after objs_before objs_after "
    "marked freed"
).split()


def run(binary, workdir, env_extra, unset=(), program=PROGRAM):
    env = {k: v for k, v in os.environ.items() if k not in unset}
    env.update(env_extra)
    src = os.path.join(workdir, "prog.lox")
    with open(src, "w") as f:
        f.write(program)
    return subprocess.run(
        [binary, src],
        cwd=workdir,
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def check_records(path, require_shrink=False):
    lines = [l.split() for l in open(path).read().splitlines() if l.strip()]
    if lines[0] != ["#", "loxpp-gc-trace", "v1"]:
        fail(f"bad header {lines[0]}")
    if lines[1][0] != "start":
        fail("no start record")
    gcs = [l for l in lines if l[0] == "gc"]
    if not gcs:
        fail("trace holds no gc records")
    if lines[-1][0] != "end":
        fail("no end record after a normal exit")
    prev_t4 = 0
    shrank = False
    for rec in gcs:
        kv = dict(x.split("=", 1) for x in rec[1:])
        if list(kv) != FIELDS:
            fail(f"gc record fields {list(kv)}")
        n = {k: int(v) for k, v in kv.items() if k != "cause"}
        if not (prev_t4 <= n["t0"] <= n["t1"] <= n["t2"] <= n["t3"] <= n["t4"]):
            fail(f"timestamps not monotonic: {kv}")
        if n["bytes_after"] > n["bytes_before"]:
            fail(f"bytes_after exceeds bytes_before: {kv}")
        if n["freed"] != n["objs_before"] - n["objs_after"]:
            fail(f"freed does not match object counts: {kv}")
        if n["marked"] != n["objs_after"]:
            fail(f"marked does not match survivors: {kv}")
        shrank = shrank or n["bytes_after"] < n["bytes_before"]
        prev_t4 = n["t4"]
    if require_shrink and not shrank:
        fail("no collection freed any bytes")
    return gcs


def main():
    binary = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory() as d:
        trace = os.path.join(d, "gc.trace")

        # Unset: nothing may be written, whether the path would be default or
        # empty-valued.
        r = run(binary, d, {}, unset=("LOXPP_GC_TRACE",))
        if r.returncode != 0:
            fail(f"plain run exited {r.returncode}: {r.stderr}")
        if os.listdir(d) != ["prog.lox"]:
            fail(f"unset run wrote files: {os.listdir(d)}")
        r = run(binary, d, {"LOXPP_GC_TRACE": ""})
        if os.listdir(d) != ["prog.lox"]:
            fail(f"empty-valued run wrote files: {os.listdir(d)}")

        # Set: the file exists and holds well-formed records.
        r = run(binary, d, {"LOXPP_GC_TRACE": trace}, unset=("LOXPP_STRESS_GC",))
        if r.returncode != 0:
            fail(f"traced run failed: {r.returncode} {r.stderr}")
        if not os.path.exists(trace):
            fail("LOXPP_GC_TRACE set but no file written")
        gcs = check_records(trace, require_shrink=True)
        if not all(g[1] == "cause=threshold" for g in gcs):
            fail("expected threshold cause without LOXPP_STRESS_GC")

        # Stress cause is reported as such.
        run(
            binary,
            d,
            {"LOXPP_GC_TRACE": trace, "LOXPP_STRESS_GC": "1"},
            program=STRESS_PROGRAM,
        )
        if not all(g[1] == "cause=stress" for g in check_records(trace)):
            fail("expected stress cause under LOXPP_STRESS_GC")

        # An unwritable path warns and the program still runs.
        r = run(binary, d, {"LOXPP_GC_TRACE": os.path.join(d, "no/such/dir/t")})
        if r.returncode != 0 or "LOXPP_GC_TRACE" not in r.stderr:
            fail("unwritable trace path must warn and not fail the run")

        # The report reads the trace.
        run(binary, d, {"LOXPP_GC_TRACE": trace}, unset=("LOXPP_STRESS_GC",))
        rep = subprocess.run(
            [sys.executable, os.path.join(HERE, "gc_report.py"), trace],
            capture_output=True,
            text=True,
        )
        if rep.returncode != 0 or "gc overhead" not in rep.stdout:
            fail(f"gc_report.py failed: {rep.stderr}")
    print("OK")


if __name__ == "__main__":
    main()
