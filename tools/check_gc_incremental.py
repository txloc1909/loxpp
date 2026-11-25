#!/usr/bin/env python3
"""Check the incremental collector (docs/decisions/garbage-collectors.md, 4).

Usage: check_gc_incremental.py <loxpp-binary>

Four parts:
  1. The probes of test/gc_incremental/. Each one moves old objects between
     large containers, writes into closures and coroutines, makes the same
     String again, or runs a deep stack or a second worker, while garbage makes
     the collector run. Each probe runs with LOXPP_GC=stw for the reference,
     and with LOXPP_GC=incremental, also with LOXPP_STRESS_GC=1 (a step at each
     allocation) and with the verifier on (LOXPP_GC_VERIFY=1, which aborts the
     process if the mark misses a reachable object). The output and the exit
     status must equal the reference. A program with a very large globals
     table is built here and checked the same way.
  2. A real trace of the incremental collector. It must have the records of
     trace format 3 in the order that the cycle has them: for each cycle one
     start, the mark steps, one finish, the sweep steps and one end; the
     number of steps of the cycle record; steps that do not overlap; a count
     of units for each step, with a fixed upper bound for a mark or sweep step.
     tools/gc_report.py must read it, count the cycles, and keep the steps of a
     cycle with cause=exit out of the pauses.
  3. A hand-written trace with an exit cycle, so the report is checked on the
     figure that no collector writes in the run of part 2 in a way that a short
     program shows.

  4. The programs of test/gc_incremental/latency/: a worker that waits many
     times with a large live heap must keep MMU(10 ms) at 50% or more, and a
     long churn on a fixed live heap must keep the peak heap within 2.5 times
     the live heap (the live heap is the largest bytes_after of a stw
     collection of the same program).

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

ROOT = os.path.dirname(HERE)
PROBES = os.path.join(ROOT, "test", "gc_incremental")
TIMEOUT_S = 300

# The limit of one mark or sweep step, from src/incremental_collector.h: the
# units of a step, one chunk of a large object, and the part of an object that
# the step cannot cut (a span of a sweep has at most about 2000 slots).
STEP_UNITS = 8192
CHUNK_UNITS = 64
MAX_STEP_UNITS = STEP_UNITS + 2 * CHUNK_UNITS + 2100

# A program with a globals table of this size, and the churn that makes the
# collector run while the roots are many.
GLOBALS = 3000


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def run(binary, prog, env_extra, cwd, timeout=TIMEOUT_S):
    env = {
        k: v
        for k, v in os.environ.items()
        if k not in ("LOXPP_GC", "LOXPP_STRESS_GC", "LOXPP_GC_VERIFY",
                     "LOXPP_GC_TRACE")
    }
    env.update(env_extra)
    try:
        return subprocess.run(
            [binary, prog], env=env, cwd=cwd, capture_output=True, text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        fail(f"{os.path.basename(prog)} {env_extra}: no end in {timeout} s")
    except OSError as exc:
        refuse_unrunnable(binary, exc)


def globals_program():
    lines = ["class Node { init(i) { this.i = i; } }"]
    for i in range(GLOBALS):
        lines.append(f"var g{i} = Node({i});")
    lines.append("var i = 0;")
    lines.append("while (i < 20000) { var x = Node(i); i = i + 1; }")
    lines.append("var sum = 0;")
    for i in range(0, GLOBALS, 7):
        lines.append(f"sum = sum + g{i}.i;")
    lines.append("print sum;")
    return "\n".join(lines) + "\n"


def check_programs(binary, work):
    probes = sorted(
        os.path.join(PROBES, f) for f in os.listdir(PROBES) if f.endswith(".lox")
    )
    if not probes:
        fail(f"no probes in {PROBES}")
    big = os.path.join(work, "globals.lox")
    with open(big, "w") as f:
        f.write(globals_program())
    probes.append(big)
    for prog in probes:
        name = os.path.basename(prog)
        ref = run(binary, prog, {"LOXPP_GC": "stw"}, work)
        if ref.returncode != 0 or not ref.stdout.strip():
            fail(f"{name}: the reference run gave exit {ref.returncode}, "
                 f"stdout {ref.stdout!r}, stderr {ref.stderr[:200]!r}")
        # A probe with a name that ends in _big needs the default pacing: a
        # step at each allocation does not end on a heap of this size.
        modes = [("incremental", {})]
        if not name.endswith("_big.lox"):
            modes.append(("incremental, stress", {"LOXPP_STRESS_GC": "1"}))
        for label, extra in modes:
            env = {"LOXPP_GC": "incremental", "LOXPP_GC_VERIFY": "1", **extra}
            r = run(binary, prog, env, work)
            if r.returncode != 0 or r.stdout != ref.stdout:
                fail(f"{name} with {label}: exit {r.returncode}, stdout "
                     f"{r.stdout[:200]!r} (reference {ref.stdout[:200]!r}), "
                     f"stderr {r.stderr[:300]!r}")
        if name.endswith("_big.lox"):
            print(f"ok   {name}")
            continue
        # The stress run must really mix the program and the collector: many
        # cycles, and each one has steps.
        trace = os.path.join(work, name + ".trace")
        r = run(binary, prog, {"LOXPP_GC": "incremental", "LOXPP_STRESS_GC": "1",
                               "LOXPP_GC_VERIFY": "1", "LOXPP_GC_TRACE": trace},
                work)
        t = gc_report.parse_records(trace)
        done = [c for c in t["cycles"] if c["cause"] == "stress"]
        if len(done) < 5:
            fail(f"{name}: {len(done)} cycles in the stress run, at least 5 "
                 "needed")
        print(f"ok   {name}: {len(done)} stress cycles")


# A program that holds a large live heap and makes garbage. It makes more
# than one cycle at the default threshold.
TRACE_PROGRAM = """\
class Node { init(i) { this.i = i; } }
var keep = [];
var i = 0;
while (i < 120000) { keep.append(Node(i)); i = i + 1; }
var m = {};
i = 0;
while (i < 30000) { m["k" + str(i)] = Node(i); i = i + 1; }
i = 0;
while (i < 600000) { keep[(i * 7919) % 120000] = Node(i); i = i + 1; }
print len(keep) + len(m);
"""


def check_trace(binary, work):
    prog = os.path.join(work, "trace_prog.lox")
    with open(prog, "w") as f:
        f.write(TRACE_PROGRAM)
    trace = os.path.join(work, "real.trace")
    r = run(binary, prog, {"LOXPP_GC": "incremental", "LOXPP_GC_TRACE": trace},
            work)
    if r.returncode != 0 or r.stdout.strip() != "150000":
        fail(f"the trace program gave exit {r.returncode}, stdout {r.stdout!r}, "
             f"stderr {r.stderr[:200]!r}")
    t = gc_report.parse_records(trace)
    cycles, steps = t["cycles"], t["steps"]
    if t["gcs"]:
        fail(f"the incremental collector wrote {len(t['gcs'])} gc records")
    if len([c for c in cycles if c["cause"] == "threshold"]) < 2:
        fail(f"{len(cycles)} cycles, at least 2 threshold cycles needed")
    if not steps:
        fail("no step records")
    whole = [c for c in cycles if c["cause"] == "threshold"]
    if len(whole) < 2:
        fail("fewer than 2 threshold cycles")
    by_cycle = {}
    for x in steps:
        by_cycle.setdefault(x["cycle"], []).append(x)
    if len(by_cycle) != len(cycles):
        fail(f"{len(by_cycle)} cycles of steps, {len(cycles)} cycle records")
    order = {"start": 0, "mark": 1, "finish": 2, "sweep": 3, "end": 4}
    for n, (cid, c) in enumerate(zip(sorted(by_cycle), cycles), 1):
        xs = by_cycle[cid]
        kinds = [x["kind"] for x in xs]
        if kinds.count("start") != 1 or kinds.count("finish") > 1 \
                or kinds.count("end") != 1:
            fail(f"cycle {cid}: kinds {kinds[:6]}... need one start, one end, "
                 "at most one finish")
        if kinds != sorted(kinds, key=lambda k: order[k]):
            fail(f"cycle {cid}: the steps are not in the order of a cycle")
        # A cycle that a full collection cuts short has no finish and no
        # sweep, so those are checked only for the cycles that have a sweep.
        if "sweep" in kinds and "finish" not in kinds:
            fail(f"cycle {cid}: a sweep with no finish pause")
        if c["steps"] != len(xs):
            fail(f"cycle {cid}: the record says {c['steps']} steps, "
                 f"{len(xs)} step records")
        if c["t0"] != xs[0]["t0"] or c["t1"] != xs[-1]["t1"]:
            fail(f"cycle {cid}: the cycle record does not span its steps")
        for a, b in zip(xs, xs[1:]):
            if b["t0"] < a["t1"]:
                fail(f"cycle {cid}: steps overlap ({a['kind']}, {b['kind']})")
        for x in xs:
            if x["kind"] in ("mark", "sweep"):
                if x["units"] < 1:
                    fail(f"cycle {cid}: a {x['kind']} step with no units")
                if c["cause"] != "exit" and x["units"] > MAX_STEP_UNITS:
                    fail(f"cycle {cid}: a {x['kind']} step of {x['units']} "
                         f"units, over {MAX_STEP_UNITS}")
        if c["objs_after"] != c["marked"]:
            fail(f"cycle {cid}: marked {c['marked']}, objs_after "
                 f"{c['objs_after']}")
    s = gc_report.summarise(trace, [1, 10])
    if s["collections"] != len(cycles):
        fail(f"the report counts {s['collections']} collections, "
             f"{len(cycles)} cycles")
    exits = [c for c in cycles if c["cause"] == "exit"]
    exit_n = sum(len(by_cycle[cid]) for cid, c in zip(sorted(by_cycle), cycles)
                 if c["cause"] == "exit")
    want = len(steps) - exit_n
    if s["steps"] != want:
        fail(f"the report counts {s['steps']} steps, {want} are not exit steps")
    if exits and (not s["exit_steps"] or s["exit_steps"]["count"] != exit_n):
        fail(f"the report has no line for the {exit_n} steps of the exit cycle")
    print(f"ok   real trace: {len(cycles)} cycles, {len(steps)} steps, "
          f"largest {s['max_step_units']} units")


HAND = """\
# loxpp-gc-trace v3
start t=0
step cycle=1 kind=start t0=1000000 t1=1100000 units=5
step cycle=1 kind=mark t0=2000000 t1=2100000 units=300
step cycle=1 kind=finish t0=3000000 t1=3050000 units=2
step cycle=1 kind=sweep t0=4000000 t1=4100000 units=200
step cycle=1 kind=end t0=4100000 t1=4110000 units=0
cycle cause=threshold t0=1000000 t1=4110000 steps=5 bytes_before=2000 bytes_after=1000 objs_before=20 objs_after=10 marked=10 freed=10
step cycle=2 kind=start t0=6000000 t1=6100000 units=3
step cycle=2 kind=mark t0=6100000 t1=9100000 units=900000
step cycle=2 kind=finish t0=9100000 t1=9200000 units=1
step cycle=2 kind=sweep t0=9200000 t1=9900000 units=800000
step cycle=2 kind=end t0=9900000 t1=9910000 units=0
cycle cause=exit t0=6000000 t1=9910000 steps=5 bytes_before=1000 bytes_after=900 objs_before=10 objs_after=9 marked=9 freed=1
end t=10000000
"""


def check_exit_cycle(work):
    path = os.path.join(work, "exit.trace")
    with open(path, "w") as f:
        f.write(HAND)
    s = gc_report.summarise(path, [1, 10])
    if s["collections"] != 2:
        fail(f"hand trace: {s['collections']} collections, not 2")
    if s["steps"] != 5 or s["max_step_units"] != 300:
        fail(f"hand trace: {s['steps']} steps, largest {s['max_step_units']}: "
             "the exit steps are in the figures of the program")
    if abs(s["pause_max_us"] - 100.0) > 1e-6:
        fail(f"hand trace: the longest pause is {s['pause_max_us']} us, "
             "not 100: an exit step counts as a pause")
    e = s["exit_steps"]
    if not e or e["count"] != 5 or e["max_units"] != 900000 \
            or abs(e["max_us"] - 3000.0) > 1e-6:
        fail(f"hand trace: the exit line is {e}")
    if "exit cycle" not in gc_report.render(s):
        fail("hand trace: the report has no exit cycle line")
    print("ok   hand trace with an exit cycle")


# The goal of the latency probes (docs/decisions/garbage-collectors.md, goals 1
# and 3): MMU(10 ms) of a program that waits, and the heap of a long churn.
MIN_MMU_10MS = 0.5
MAX_PEAK_OVER_LIVE = 2.5


def trace_fields(path, kind):
    out = []
    with open(path) as f:
        for line in f:
            if line.startswith(kind + " "):
                out.append(dict(x.split("=", 1) for x in line.split()[1:]))
    return out


def check_idle_latency(binary, work):
    prog = os.path.join(PROBES, "latency", "idle_wait.lox")
    # The windows of a run depend on the host, so a run that fails once is
    # run again: a collector that runs its cycle at idle fails every run.
    worst = None
    for attempt in range(3):
        trace = os.path.join(work, f"idle_wait.{attempt}.trace")
        r = run(binary, prog, {"LOXPP_GC": "incremental",
                               "LOXPP_GC_TRACE": trace}, work)
        if r.returncode != 0:
            fail(f"idle_wait.lox: exit {r.returncode}, {r.stderr[:200]!r}")
        mmu = gc_report.summarise(trace, [10])["mmu"]["10"]
        worst = mmu if worst is None else max(worst, mmu)
        if mmu >= MIN_MMU_10MS:
            print(f"ok   idle_wait.lox: MMU(10 ms) {100 * mmu:.1f}%")
            return
    fail(f"idle_wait.lox: MMU(10 ms) {100 * worst:.1f}% in 3 runs, at least "
         f"{100 * MIN_MMU_10MS:.0f}% needed: a worker that goes idle runs "
         "the steps of a cycle with no break")


def check_peak(binary, work):
    prog = os.path.join(PROBES, "latency", "churn_peak.lox")
    ref = os.path.join(work, "churn_peak.stw.trace")
    r = run(binary, prog, {"LOXPP_GC": "stw", "LOXPP_GC_TRACE": ref}, work)
    if r.returncode != 0:
        fail(f"churn_peak.lox with stw: exit {r.returncode}")
    live = max(int(g["bytes_after"]) for g in trace_fields(ref, "gc")
               if g["cause"] == "threshold")
    trace = os.path.join(work, "churn_peak.inc.trace")
    r = run(binary, prog, {"LOXPP_GC": "incremental", "LOXPP_GC_TRACE": trace},
            work)
    if r.returncode != 0:
        fail(f"churn_peak.lox with incremental: exit {r.returncode}")
    peak = max(int(c["bytes_peak"]) for c in trace_fields(trace, "cycle")
               if c["cause"] != "exit")
    if peak > MAX_PEAK_OVER_LIVE * live:
        fail(f"churn_peak.lox: the heap peaked at {peak} bytes, "
             f"{peak / live:.2f} times the live heap of {live} bytes "
             f"(at most {MAX_PEAK_OVER_LIVE})")
    print(f"ok   churn_peak.lox: peak {peak / live:.2f} times the live heap")


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    require_runner(sys.argv[1])
    if os.environ.get("LOXPP_EXEC_TRACE_ENABLED") == "1":
        print("skipped: build traces execution")
        sys.exit(125)
    binary = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory() as work:
        check_programs(binary, work)
        check_trace(binary, work)
        check_exit_cycle(work)
        check_idle_latency(binary, work)
        check_peak(binary, work)
    print("OK")


if __name__ == "__main__":
    main()
