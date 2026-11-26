#!/usr/bin/env python3
"""Check the reader of the GC trace format, version 3 (tools/gc_report.py).

Usage: check_gc_trace_v3.py <loxpp-binary>

Four parts:
  1. A hand-written v3 trace with gc, cycle, step and stall records. The
     report must give exact figures. The stop-the-world collector writes gc
     records only, so this is the only check that shows the reader handles
     the records that an incremental collector writes.
  2. A hand-written v2 trace. It must read as before.
  3. Malformed files. Each one must be rejected with a message that names the
     file and the line.
  4. A real run of the binary. Its trace, without stall records and with the
     header changed to v2, must give the same report as the v3 file, on every
     figure that both have.

Exit status: 0 pass, 1 fail, 2 usage error or a binary that cannot run.
"""

import os
import subprocess
import sys
import tempfile

from runner_precondition import refuse_unrunnable, require_runner

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gc_report  # noqa: E402

TIMEOUT_S = 120

# Times are in ns. One start pause of 100 us, two mark steps of 200 and
# 300 us with a gap of 1 ms between them, a finish pause of 50 us that touches
# the second mark step, one sweep step and the end of the sweep. Three stalls:
# one inside the sweep step, which a report that adds intervals without
# merging them would count twice.
V3_TRACE = """\
# loxpp-gc-trace v3
start t=0
step cycle=1 kind=start t0=1000000 t1=1100000 units=7
step cycle=1 kind=mark t0=2000000 t1=2200000 units=900
step cycle=1 kind=mark t0=3200000 t1=3500000 units=1200
step cycle=1 kind=finish t0=3500000 t1=3550000 units=3
step cycle=1 kind=sweep t0=4000000 t1=4400000 units=1000
step cycle=1 kind=end t0=4400000 t1=4410000 units=0
cycle cause=threshold t0=1000000 t1=4410000 steps=6 bytes_before=2000 bytes_after=1000 objs_before=20 objs_after=10 marked=10 freed=10
stall kind=free t0=4100000 t1=4300000
stall kind=free t0=5000000 t1=5700000
stall kind=new t0=6000000 t1=6100000
gc cause=exit t0=8000000 t1=8000100 t2=8000200 t3=8000300 t4=8000400 bytes_before=1000 bytes_after=900 objs_before=10 objs_after=9 marked=9 freed=1
end t=10000000
"""

V2_TRACE = """\
# loxpp-gc-trace v2
start t=0
gc cause=threshold t0=1000000 t1=1200000 t2=1500000 t3=1600000 t4=2000000 bytes_before=2000 bytes_after=1000 objs_before=20 objs_after=10 marked=10 freed=10
gc cause=exit t0=5000000 t1=5000100 t2=5000200 t3=5000300 t4=5000400 bytes_before=1000 bytes_after=900 objs_before=10 objs_after=9 marked=9 freed=1
end t=10000000
"""

STEP = "step cycle=1 kind=mark t0=100 t1=200 units=5\n"
STALL = "stall kind=new t0=100 t1=200\n"
CYCLE = ("cycle cause=threshold t0=1 t1=2 steps=1 bytes_before=2 bytes_after=1 "
         "objs_before=2 objs_after=1 marked=1 freed=1\n")
GC = ("gc cause=threshold t0=1 t1=2 t2=3 t3=4 t4=5 bytes_before=2 "
      "bytes_after=1 objs_before=2 objs_after=1 marked=1 freed=1\n")
V3H = "# loxpp-gc-trace v3\nstart t=0\n"
V2H = "# loxpp-gc-trace v2\nstart t=0\n"

# (name, file text, a text that the error message must contain)
BAD = [
    ("unknown record", V3H + "frob t=1\n", "unknown record 'frob'"),
    ("step without units", V3H + "step cycle=1 kind=mark t0=1 t1=2\n", "no units"),
    ("step ends before it starts", V3H + "step cycle=1 kind=mark t0=5 t1=2 units=1\n",
     "ends before it starts"),
    ("step with an unknown kind", V3H + "step cycle=1 kind=nap t0=1 t1=2 units=1\n",
     "step kind 'nap'"),
    ("step with a negative count", V3H + "step cycle=1 kind=mark t0=1 t1=2 units=-3\n",
     "negative"),
    ("stall with an unknown kind", V3H + "stall kind=malloc t0=1 t1=2\n",
     "stall kind 'malloc'"),
    ("cycle without a field", V3H + CYCLE.replace(" freed=1", ""), "no freed"),
    ("gc without a field", V3H + GC.replace(" bytes_after=1", ""), "no bytes_after"),
    ("a field that is not an integer", V3H + GC.replace("t0=1", "t0=abc"),
     "not an integer"),
    ("a token that is not key=value", V3H + "step cycle=1 mark\n", "not key=value"),
    ("a step in a v2 trace", V2H + STEP, "step record in a version 2 trace"),
    ("a stall in a v2 trace", V2H + STALL, "stall record in a version 2 trace"),
    ("a cycle in a v2 trace", V2H + CYCLE, "cycle record in a version 2 trace"),
    ("a version above 3", "# loxpp-gc-trace v4\nstart t=0\n", "newer than 3"),
    ("no start record", "# loxpp-gc-trace v3\n" + GC, "no start record"),
]


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def write(d, name, text):
    path = os.path.join(d, name)
    with open(path, "w") as f:
        f.write(text)
    return path


def close(a, b):
    return abs(a - b) < 1e-6


def check_v3_figures(d):
    s = gc_report.summarise(write(d, "v3.trace", V3_TRACE), [1, 10])
    want = {
        "collections": 2,  # the cycle and the gc record
        "steps": 6,
        "max_step_units": 1200,
        "pause_max_us": 400.0,  # the sweep step, 400 us
    }
    for k, v in want.items():
        if s[k] != v:
            fail(f"v3 report {k} is {s[k]!r}, not {v!r}")
    # Merged pauses: 100 us, 200 us, 300+50 us touching, 400+10 us touching,
    # and the gc record of 400 ns. The sum is 1.06 ms and 0.0004 ms.
    if not close(s["gc_ms"], 1.0604):
        fail(f"v3 report gc_ms is {s['gc_ms']}, not 1.0604")
    if s["stalls"] != {"free": {"count": 2, "max_us": 700.0},
                       "new": {"count": 1, "max_us": 100.0}}:
        fail(f"v3 report stalls are {s['stalls']}")
    # Worst 10 ms window without stalls: it holds 100+200+350+410 us of pause
    # and the gc record, 1.06 ms, so the program runs for 89.4% of it.
    if not close(s["mmu"]["10"], 1 - 1.0604 / 10):
        fail(f"v3 report MMU(10 ms) is {s['mmu']['10']}")
    # With the stalls (0.7 ms and 0.1 ms) 1.8604 ms is paused in 10 ms.
    if not s["mmu_with_stalls"] or not close(
            s["mmu_with_stalls"]["10"], 1 - 1.8604 / 10):
        fail(f"v3 report MMU(10 ms) with stalls is {s['mmu_with_stalls']}")
    # An MMU window of 1 ms must see the sweep step of 400 us touching the
    # end step of 10 us as one pause of 410 us.
    if s["mmu"]["1"] > 1 - 0.41 + 1e-6:
        fail(f"v3 report MMU(1 ms) is {s['mmu']['1']}: touching steps not merged")
    text = gc_report.render(s)
    for needle in ("steps             6  max units 1200", "MMU with stalls",
                   "stall free", "stall new"):
        if needle not in text:
            fail(f"the rendered report has no '{needle}':\n{text}")


def check_v2_figures(d):
    s = gc_report.summarise(write(d, "v2.trace", V2_TRACE), [10])
    if s["collections"] != 2 or s["steps"] != 0 or s["stalls"] != {}:
        fail(f"v2 report has {s['collections']} collections, {s['steps']} steps")
    if s["max_step_units"] is not None or s["mmu_with_stalls"] is not None:
        fail("v2 report has step or stall figures")
    if not close(s["pause_max_us"], 1000.0) or not close(s["gc_ms"], 1.0004):
        fail(f"v2 report pauses: max {s['pause_max_us']}, total {s['gc_ms']}")
    # trace phase: (t2 - t1) of both records over the total gc time.
    if s["phase_pct"] is None or not close(
            s["phase_pct"]["trace"], 100.0 * (300000 + 100) / 1000400):
        fail(f"v2 report phase shares are {s['phase_pct']}")
    start, end, gcs = gc_report.parse_trace(os.path.join(d, "v2.trace"))
    if (start, end, len(gcs)) != (0, 10000000, 2):
        fail("parse_trace no longer gives the version 2 view")


def check_rejections(d):
    for name, text, needle in BAD:
        path = write(d, "bad.trace", text)
        try:
            gc_report.summarise(path, [10])
        except ValueError as exc:
            msg = str(exc)
            if needle not in msg:
                fail(f"{name}: the message '{msg}' has no '{needle}'")
            if not msg.startswith(path + ":") and "no start record" not in msg:
                fail(f"{name}: the message '{msg}' does not name the file and line")
        else:
            fail(f"{name}: the reader accepted the file")


def check_real_run(binary, d):
    prog = write(d, "prog.lox", 'var s = "x";\nfor (var i = 0; i < 22; i = i + 1) { s = s + s; }\n')
    trace = os.path.join(d, "real.trace")
    env = {k: v for k, v in os.environ.items() if k != "LOXPP_STRESS_GC"}
    env["LOXPP_GC"] = "stw"
    env["LOXPP_GC_TRACE"] = trace
    try:
        r = subprocess.run([binary, prog], env=env, cwd=d, capture_output=True,
                           text=True, timeout=TIMEOUT_S)
    except OSError as exc:
        refuse_unrunnable(binary, exc)
    if r.returncode != 0:
        fail(f"traced run failed: {r.stderr}")
    text = open(trace).read()
    if not text.startswith("# loxpp-gc-trace v3\n"):
        fail(f"the writer does not write the v3 header: {text.splitlines()[0]}")
    for line in text.splitlines():
        if line.split()[0] in ("cycle", "step"):
            fail(f"the stop-the-world collector wrote '{line.split()[0]}'")
    # The heap writes a stall record when a call into the C allocator is slow,
    # so a real run may have some (tools/check_gc_stall.py forces them). A
    # version 2 file has none, so the two readings compare without them.
    text = "".join(l + "\n" for l in text.splitlines()
                   if not l.startswith("stall "))
    trace = write(d, "real_nostall.trace", text)
    v2 = write(d, "real_v2.trace", text.replace("v3", "v2", 1))
    a = gc_report.summarise(trace, [10, 100])
    b = gc_report.summarise(v2, [10, 100])
    for k in a:
        if k == "file":
            continue
        if a[k] != b[k]:
            fail(f"the report of one run differs for {k}: v3 {a[k]!r}, v2 {b[k]!r}")


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    require_runner(sys.argv[1])
    binary = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory() as d:
        check_v3_figures(d)
        check_v2_figures(d)
        check_rejections(d)
        check_real_run(binary, d)
        out = subprocess.run(
            [sys.executable, os.path.join(HERE, "gc_report.py"),
             os.path.join(d, "v3.trace")], capture_output=True, text=True)
        if out.returncode != 0 or "stall free" not in out.stdout:
            fail(f"gc_report.py failed on the v3 file: {out.stderr}")
    print("OK")


if __name__ == "__main__":
    main()
