#!/usr/bin/env python3
"""Summarise a LOXPP_GC_TRACE file.

Usage: gc_report.py [--json] [--mmu-windows MS,MS,...] TRACE [TRACE...]

Producing a trace (any build; the trace is not tied to a build flag):

    LOXPP_GC_TRACE=/tmp/gc.trace build/loxpp program.lox

Trace format, version 1
-----------------------
A text file, one record per line. Blank lines and lines that begin with '#'
are ignored. Every other line is a record name followed by space-separated
key=value fields. All t* values are CLOCK_MONOTONIC nanoseconds, so they are
comparable inside one file and meaningless across files or hosts.

    # loxpp-gc-trace v1          header; the version bumps on any change
    start t=<ns>                 the MemoryManager was created
    gc cause=<threshold|stress>  one record per collection, with fields:
       t0..t4          t0 collection begins; t1 roots marked; t2 gray stack
                       traced; t3 unmarked interned strings removed; t4 sweep
                       finished. Phases: mark = t1-t0, trace = t2-t1,
                       strings = t3-t2, sweep = t4-t3, pause = t4-t0.
       bytes_before/bytes_after   bytesAllocated at t0 and t4
       objs_before/objs_after     heap object count at t0 and t4
       marked          objects found reachable (equals objs_after)
       freed           objects swept (objs_before - objs_after)
    end t=<ns>                   the MemoryManager was destroyed; absent when
                                 the process left through std::exit(), and
                                 the last t4 stands in for it then

Metric definitions
------------------
wall            end (or last t4) minus start.
GC overhead     total pause time over wall.
MMU(w)          minimum mutator utilisation: the smallest fraction of any
                window of length w that lies outside a pause. Windows are
                tried from every pause start and to every pause end, which is
                where the minimum occurs.
alloc rate      bytes allocated between collections over wall. Allocation
                after the last collection is not seen, so this is a lower
                bound.
mark rate       marked objects over time in the mark+trace phases.
sweep rate      objects visited by the sweep over time in the sweep phase.
"""

import argparse
import bisect
import json
import sys

PHASES = ("mark", "trace", "strings", "sweep")
DEFAULT_MMU_MS = (1, 10, 100, 1000)


def parse_trace(path):
    start = end = None
    gcs = []
    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, *fields = line.split()
            kv = dict(x.split("=", 1) for x in fields)
            if name == "start":
                start = int(kv["t"])
            elif name == "end":
                end = int(kv["t"])
            elif name == "gc":
                rec = {k: (v if k == "cause" else int(v)) for k, v in kv.items()}
                gcs.append(rec)
            else:
                raise ValueError(f"{path}:{n}: unknown record '{name}'")
    if start is None:
        raise ValueError(f"{path}: no start record")
    if end is None:
        end = gcs[-1]["t4"] if gcs else start
    return start, end, gcs


def percentile(sorted_vals, p):
    if not sorted_vals:
        return 0
    idx = max(0, min(len(sorted_vals) - 1, -(-len(sorted_vals) * p // 100) - 1))
    return sorted_vals[int(idx)]


def mmu(start, end, pauses, window):
    """pauses: list of (begin, finish) sorted, non-overlapping."""
    if window > end - start or not pauses:
        return None if window > end - start else 1.0
    begins = [b for b, _ in pauses]
    prefix = [0]
    for b, e in pauses:
        prefix.append(prefix[-1] + (e - b))

    def paused_before(t):
        # Pause time in [start, t].
        i = bisect.bisect_right(begins, t)
        total = prefix[i - 1] if i else 0
        if i:
            b, e = pauses[i - 1]
            total += max(0, min(e, t) - b)
        return total

    worst = 0
    cands = [b for b, _ in pauses] + [e - window for _, e in pauses]
    for lo in cands:
        lo = max(start, min(lo, end - window))
        worst = max(worst, paused_before(lo + window) - paused_before(lo))
    return 1.0 - worst / window


def rate(amount, ns):
    return amount / (ns / 1e9) if ns > 0 else 0.0


def summarise(path, mmu_ms):
    start, end, gcs = parse_trace(path)
    wall = max(end - start, 1)
    pauses_ns = [g["t4"] - g["t0"] for g in gcs]
    sorted_p = sorted(pauses_ns)
    total_pause = sum(pauses_ns)
    phase_ns = {
        "mark": sum(g["t1"] - g["t0"] for g in gcs),
        "trace": sum(g["t2"] - g["t1"] for g in gcs),
        "strings": sum(g["t3"] - g["t2"] for g in gcs),
        "sweep": sum(g["t4"] - g["t3"] for g in gcs),
    }
    allocated = 0
    prev_after = 0
    for g in gcs:
        allocated += max(0, g["bytes_before"] - prev_after)
        prev_after = g["bytes_after"]
    marked = sum(g["marked"] for g in gcs)
    swept_visited = sum(g["objs_before"] for g in gcs)
    freed = sum(g["freed"] for g in gcs)
    intervals = [(g["t0"], g["t4"]) for g in gcs]
    causes = {}
    for g in gcs:
        causes[g["cause"]] = causes.get(g["cause"], 0) + 1
    return {
        "file": path,
        "collections": len(gcs),
        "causes": causes,
        "wall_ms": wall / 1e6,
        "gc_ms": total_pause / 1e6,
        "gc_overhead_pct": 100.0 * total_pause / wall,
        "pause_p50_us": percentile(sorted_p, 50) / 1e3,
        "pause_p99_us": percentile(sorted_p, 99) / 1e3,
        "pause_max_us": (sorted_p[-1] if sorted_p else 0) / 1e3,
        "phase_pct": {
            k: (100.0 * v / total_pause if total_pause else 0.0)
            for k, v in phase_ns.items()
        },
        "mmu": {str(ms): mmu(start, end, intervals, ms * 1_000_000) for ms in mmu_ms},
        "alloc_mb_per_s": rate(allocated, wall) / 1e6,
        "mark_mobj_per_s": rate(marked, phase_ns["mark"] + phase_ns["trace"]) / 1e6,
        "sweep_mobj_per_s": rate(swept_visited, phase_ns["sweep"]) / 1e6,
        "objects_freed": freed,
    }


def render(s):
    out = [f"== {s['file']}"]
    causes = ", ".join(f"{k}={v}" for k, v in sorted(s["causes"].items()))
    out.append(f"collections       {s['collections']}  ({causes or 'none'})")
    out.append(f"wall              {s['wall_ms']:.1f} ms")
    out.append(f"gc time           {s['gc_ms']:.2f} ms")
    out.append(f"gc overhead       {s['gc_overhead_pct']:.2f} % of wall")
    out.append(
        f"pause p50/p99/max {s['pause_p50_us']:.1f} / {s['pause_p99_us']:.1f}"
        f" / {s['pause_max_us']:.1f} us"
    )
    out.append(
        "phase share       "
        + "  ".join(f"{k} {s['phase_pct'][k]:.1f}%" for k in PHASES)
    )
    mm = "  ".join(
        f"{ms}ms:{'n/a' if v is None else f'{100 * v:.1f}%'}"
        for ms, v in s["mmu"].items()
    )
    out.append(f"MMU               {mm}")
    out.append(f"alloc rate        {s['alloc_mb_per_s']:.1f} MB/s")
    out.append(f"mark rate         {s['mark_mobj_per_s']:.2f} Mobj/s")
    out.append(f"sweep rate        {s['sweep_mobj_per_s']:.2f} Mobj/s")
    return "\n".join(out)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("traces", nargs="+")
    ap.add_argument("--json", action="store_true", help="emit one JSON list")
    ap.add_argument(
        "--mmu-windows",
        default=",".join(map(str, DEFAULT_MMU_MS)),
        help="comma-separated MMU window sizes in ms",
    )
    args = ap.parse_args(argv)
    mmu_ms = [float(x) if "." in x else int(x) for x in args.mmu_windows.split(",")]
    results = [summarise(p, mmu_ms) for p in args.traces]
    if args.json:
        json.dump(results, sys.stdout, indent=2)
        print()
    else:
        print("\n\n".join(render(r) for r in results))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
