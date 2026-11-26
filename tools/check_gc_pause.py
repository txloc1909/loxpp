#!/usr/bin/env python3
"""Gate goal 1 of docs/decisions/garbage-collectors.md for `incremental`.

Usage: check_gc_pause.py <loxpp-binary> [--runs N]
       check_gc_pause.py --trace <file> [--trace <file>]... [--label <name>]
       check_gc_pause.py --self-test

The first form runs each program of benchmarks/gc_large_heap/ with its full
constants and the incremental collector, N times (default 3), and gates the
GC trace of each run. The second form gates traces that another tool wrote,
such as the one that the Redis load writes (gc_load.py, in the tests
directory of the Redis example). The third form checks the gate itself on hand-written traces.

Two gates, in this order:

  1. A count, from one run. No collector step that belongs to the program
     (a step of a cycle with cause=exit runs after the program ends) has more
     than MAX_STEP_UNITS work units. The count does not depend on the host.
     The run must also make at least MIN_CYCLES cycles and no stop-the-world
     record: a program that never collects, or that ran the other collector,
     measures nothing.
  2. Time, from the best of the runs of a program, because a shared host
     stalls a process for more than 1 ms now and then: the 99.99th percentile
     of the steps is at most MAX_P9999_US, and MMU(10 ms) from the steps is at
     least MIN_MMU_10MS.

The allocator stall is not part of goal 1. It is printed on its own, with MMU
with and without it.

Exit status: 0 pass, 1 fail, 2 usage error or a binary that cannot run, 125
skipped (a build that traces execution prints too much to run these programs).
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

from runner_precondition import refuse_unrunnable, require_runner

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_gc_incremental as inc  # noqa: E402
import check_gc_large_heap as large  # noqa: E402
import gc_report  # noqa: E402

# One step of the collector does at most this many units: the limit of a step,
# two chunks of a large object and the span of a sweep.
MAX_STEP_UNITS = inc.MAX_STEP_UNITS
MIN_CYCLES = 3
MAX_P9999_US = 1000.0
MIN_MMU_10MS = 0.5
TIMEOUT_S = 600


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def measure(path):
    """The figures of one trace, with the count failures."""
    t = gc_report.parse_records(path)
    s = gc_report.summarise(path, [10])
    own = [c for c in t["cycles"] if c["cause"] != "exit"]
    exit_iv = [(c["t0"], c["t1"]) for c in t["cycles"] if c["cause"] == "exit"]
    steps = [
        x for x in t["steps"]
        if not any(a <= x["t0"] and x["t1"] <= b for a, b in exit_iv)
    ]
    worst = {}
    for x in steps:
        worst[x["kind"]] = max(worst.get(x["kind"], 0), x["units"])
    problems = []
    if t["gcs"]:
        problems.append(
            f"{len(t['gcs'])} stop-the-world records: the collector was not "
            "incremental"
        )
    if len(own) < MIN_CYCLES:
        problems.append(f"{len(own)} cycles, at least {MIN_CYCLES} needed")
    if not steps:
        problems.append("no step records")
    largest = max(worst.values(), default=0)
    if largest > MAX_STEP_UNITS:
        kind = max(worst, key=worst.get)
        problems.append(
            f"a {kind} step of {largest} units, over {MAX_STEP_UNITS}"
        )
    stall = {k: v for k, v in s["stalls"].items()}
    return {
        "cycles": len(own),
        "steps": len(steps),
        "largest_units": largest,
        "units_by_kind": worst,
        "p9999_us": s["pause_p9999_us"],
        "max_us": s["pause_max_us"],
        "mmu10": s["mmu"]["10"],
        "mmu10_stalls": (s["mmu_with_stalls"] or s["mmu"])["10"],
        "stalls": stall,
        "problems": problems,
    }


def pct(v):
    return "n/a" if v is None else f"{100 * v:.1f}%"


def describe(label, m):
    stalls = ", ".join(
        f"{k} {v['count']} (max {v['max_us']:.0f} us)" for k, v in m["stalls"].items()
    ) or "none"
    return (
        f"{label}: cycles {m['cycles']}, steps {m['steps']}, largest step "
        f"{m['largest_units']} units {m['units_by_kind']}, p99.99 "
        f"{m['p9999_us']:.0f} us, max step {m['max_us']:.0f} us, MMU(10 ms) "
        f"{pct(m['mmu10'])} (with stalls {pct(m['mmu10_stalls'])}), allocator "
        f"stalls: {stalls}"
    )


def gate(label, runs, show=True):
    """Gates the figures of one program. Returns a list of failure messages.

    The count gate holds for every run. The time gate holds for the best run:
    the lowest p99.99 and the highest MMU, each on its own.
    """
    out = []
    for i, m in enumerate(runs, 1):
        if show:
            print(describe(f"{label} run {i}", m))
        for p in m["problems"]:
            out.append(f"{label} run {i}: {p}")
    best_p = min(m["p9999_us"] for m in runs)
    best_mmu = max((m["mmu10"] for m in runs if m["mmu10"] is not None),
                   default=None)
    if show:
        print(f"{label}: best of {len(runs)}: p99.99 {best_p:.0f} us (limit "
              f"{MAX_P9999_US:.0f}), MMU(10 ms) {pct(best_mmu)} (limit "
              f"{100 * MIN_MMU_10MS:.0f}%)")
    if best_p > MAX_P9999_US:
        out.append(f"{label}: p99.99 of the steps {best_p:.0f} us in every "
                   f"run, over {MAX_P9999_US:.0f}")
    if best_mmu is not None and best_mmu < MIN_MMU_10MS:
        out.append(f"{label}: MMU(10 ms) {pct(best_mmu)} in every run, under "
                   f"{100 * MIN_MMU_10MS:.0f}%")
    return out


def full_constants(source):
    return {
        k: int(v)
        for k, v in re.findall(r"^var ([A-Z]+) = (\d+);", source, flags=re.M)
    }


def run_programs(binary, runs):
    env = {
        k: v for k, v in os.environ.items()
        if k not in ("LOXPP_GC", "LOXPP_STRESS_GC", "LOXPP_GC_VERIFY",
                     "LOXPP_GC_TRACE")
    }
    env["LOXPP_GC"] = "incremental"
    on_disk = sorted(f[:-4] for f in os.listdir(large.PROGRAMS)
                     if f.endswith(".lox"))
    if on_disk != sorted(large.CASES):
        fail(f"programs on disk {on_disk} differ from the cases "
             f"{sorted(large.CASES)}")
    failures = []
    with tempfile.TemporaryDirectory() as d:
        for name in on_disk:
            prog = os.path.join(large.PROGRAMS, f"{name}.lox")
            with open(prog) as f:
                consts = full_constants(f.read())
            want = str(large.CASES[name][1](consts))
            measured = []
            for i in range(runs):
                trace = os.path.join(d, f"{name}.{i}.trace")
                try:
                    r = subprocess.run(
                        [binary, prog], env={**env, "LOXPP_GC_TRACE": trace},
                        cwd=d, capture_output=True, text=True,
                        timeout=TIMEOUT_S,
                    )
                except subprocess.TimeoutExpired:
                    fail(f"{name}: no end in {TIMEOUT_S} s")
                except OSError as exc:
                    refuse_unrunnable(binary, exc)
                if r.returncode != 0:
                    fail(f"{name}: exit {r.returncode}: {r.stderr[:200]}")
                if r.stdout.strip() != want:
                    fail(f"{name}: printed {r.stdout.strip()!r}, arithmetic "
                         f"gives {want}")
                measured.append(measure(trace))
            failures += gate(name, measured)
    return failures


# --- the gate on hand-written traces

def hand(units, step_ns, gap_ns, cycles=4, exit_units=0, stall_ns=0):
    """A trace with `cycles` cycles. Each has a mark step of `units` units and
    `step_ns` long, a start, a finish and an end step, and `gap_ns` of
    program time after each step."""
    lines = ["# loxpp-gc-trace v3", "start t=0"]
    t = 1_000_000
    for cid in range(1, cycles + 1):
        t0 = t
        for kind, u in (("start", 3), ("mark", units), ("finish", 1),
                        ("end", 0)):
            lines.append(f"step cycle={cid} kind={kind} t0={t} "
                         f"t1={t + step_ns} units={u}")
            t += step_ns + gap_ns
        lines.append(
            f"cycle cause=threshold t0={t0} t1={t - gap_ns} steps=4 "
            "bytes_before=2000 bytes_after=1000 objs_before=20 "
            "objs_after=10 marked=10 freed=10"
        )
    if exit_units:
        lines.append(f"step cycle={cycles + 1} kind=mark t0={t} t1={t + 5_000_000} "
                     f"units={exit_units}")
        lines.append(
            f"cycle cause=exit t0={t} t1={t + 5_000_000} steps=1 "
            "bytes_before=1000 bytes_after=900 objs_before=10 objs_after=9 "
            "marked=9 freed=1"
        )
        t += 5_000_000
    if stall_ns:
        lines.append(f"stall kind=new t0={t} t1={t + stall_ns}")
        t += stall_ns
    # Program time after the last step, so that a 10 ms window fits.
    lines.append(f"end t={t + 100_000_000}")
    return "\n".join(lines) + "\n"


def self_test():
    cases = [
        # name, trace, whether the gate must pass
        ("small steps with a gap pass", hand(300, 100_000, 400_000), True),
        ("a step over the unit limit fails the count",
         hand(MAX_STEP_UNITS + 1, 100_000, 400_000), False),
        ("an exit step over the unit limit is not counted",
         hand(300, 100_000, 400_000, exit_units=10_000_000), True),
        ("steps over 1 ms fail the time",
         hand(300, 2_000_000, 40_000_000), False),
        ("steps with no gap fail MMU",
         hand(300, 100_000, 1_000, cycles=100), False),
        ("an allocator stall is not a pause",
         hand(300, 100_000, 400_000, stall_ns=50_000_000), True),
        ("too few cycles fail the count",
         hand(300, 100_000, 400_000, cycles=MIN_CYCLES - 1), False),
    ]
    with tempfile.TemporaryDirectory() as d:
        for name, text, passes in cases:
            path = os.path.join(d, "t.trace")
            with open(path, "w") as f:
                f.write(text)
            out = gate(name, [measure(path)])
            if bool(out) == passes:
                fail(f"self-test {name!r}: gate "
                     f"{'failed: ' + out[0] if out else 'passed'}, expected "
                     f"{'a pass' if passes else 'a failure'}")
        # The best of the runs decides the time, and every run decides the
        # count.
        slow, quick = os.path.join(d, "slow"), os.path.join(d, "quick")
        with open(slow, "w") as f:
            f.write(hand(300, 2_000_000, 40_000_000))
        with open(quick, "w") as f:
            f.write(hand(300, 100_000, 400_000))
        if gate("best of two", [measure(slow), measure(quick)]):
            fail("self-test: a slow run beside a quick run failed the gate")
        big = os.path.join(d, "big")
        with open(big, "w") as f:
            f.write(hand(MAX_STEP_UNITS + 1, 100_000, 400_000))
        if not gate("count on every run", [measure(quick), measure(big)]):
            fail("self-test: a run over the unit limit passed")
    print("OK")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("binary", nargs="?")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--trace", action="append", default=[])
    ap.add_argument("--label", default="trace")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return
    if args.trace:
        failures = gate(args.label, [measure(p) for p in args.trace])
    elif args.binary:
        if os.environ.get("LOXPP_EXEC_TRACE_ENABLED") == "1":
            print("skipped: build traces execution")
            sys.exit(125)
        require_runner(args.binary)
        failures = run_programs(os.path.abspath(args.binary), args.runs)
    else:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    if failures:
        for f in failures:
            print(f"FAIL: {f}")
        sys.exit(1)
    print("OK")


if __name__ == "__main__":
    main()
