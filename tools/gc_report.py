#!/usr/bin/env python3
"""Summarise a LOXPP_GC_TRACE file (JVM logs: tools/jvm_gc_log.py).

Usage: gc_report.py [--json] [--mmu-windows MS,MS,...] TRACE [TRACE...]

Producing a trace (any build; the trace is not tied to a build flag):

    LOXPP_GC_TRACE=/tmp/gc.trace build/loxpp program.lox

Trace format, version 3
-----------------------
A text file, one record per line. Blank lines and lines that begin with '#'
are ignored. Every other line is a record name followed by space-separated
key=value fields. All t* values are CLOCK_MONOTONIC nanoseconds, so they are
comparable inside one file and meaningless across files or hosts.

    # loxpp-gc-trace v3          header; the version bumps on any change
    start t=<ns>                 the MemoryManager was created
    gc cause=<threshold|stress|exit>
                                 one record per stop-the-world collection,
                                 with fields:
       t0..t4          t0 collection begins; t1 roots marked; t2 gray stack
                       traced; t3 unmarked interned strings removed; t4 sweep
                       finished. Phases: mark = t1-t0, trace = t2-t1,
                       strings = t3-t2, sweep = t4-t3, pause = t4-t0.
       bytes_before/bytes_after   bytesAllocated at t0 and t4
       objs_before/objs_after     heap object count at t0 and t4
       marked          objects found reachable (equals objs_after)
       freed           objects swept (objs_before - objs_after)
    cycle cause=<...> t0= t1= steps= bytes_before= bytes_after=
          objs_before= objs_after= marked= freed=
                                 one record at the end of an incremental
                                 cycle. t0 is the start of the cycle and t1
                                 its end. It is a summary, not a pause: the
                                 pauses of the cycle are its step records.
    step cycle=<n> kind=<start|mark|finish|sweep|end> t0=<ns> t1=<ns>
         units=<n>               one bounded step of a cycle, and one pause.
                                 units counts the work of the step: objects
                                 traced, slots swept, elements or entries
                                 visited.
    stall kind=<new|free> t0=<ns> t1=<ns>
                                 one call of the C allocator that took longer
                                 than a limit. It is not a collector pause.
                                 The report counts it on its own.
    end t=<ns>                   the MemoryManager was destroyed; absent when
                                 the process ended through _Exit() while the
                                 heap still existed (an exit() call, an
                                 uncaught fault, or a program end while the
                                 heap's worker ran or a holder kept its dead
                                 heap), and the last t4 stands in for it then

The last gc record of a heap whose worker ended normally has cause=exit:
the collection that worker ran as it ended, with the held messages as its
only roots. Its objs_after and bytes_after are what the heap keeps as a dead
heap while another worker holds those messages; its end record follows when
the last of them is dropped.

Each worker writes its own file: the main worker writes the path in
LOXPP_GC_TRACE, and worker N (N >= 1, in the order of task.spawn_worker
calls) writes <path>.w<N>.

Version 2 added cause=exit and the files of the workers. A version 1 file
reads the same way. Version 3 added the cycle, step and stall records. The
stop-the-world collector writes gc records only, with the same fields as
version 2. A cycle, step or stall record in a file of an older version is an
error, and so is a version above 3.

Each MemoryManager opens the file for writing and truncates it. A Lox program
that starts a child loxpp inherits the variable, so the child truncates the
parent's trace; unset the variable for the child, or give it its own path.

Metric definitions
------------------
wall            end (or last t4) minus start.
GC overhead     total pause time over wall.
pause           a gc record from t0 to t4, or a step record from t0 to t1.
                Pauses that touch are one pause for MMU.
MMU(w)          minimum mutator utilisation: the smallest fraction of any
                window of length w that lies outside a pause. Windows are
                tried from every pause start and to every pause end, which is
                where the minimum occurs. "MMU with stalls" counts the stall
                records as pauses too.
stall           the longest stall of each kind (new, free), and how many.
step units      the largest units value of one step.
alloc rate      bytes allocated between collections over wall. Allocation
                after the last collection is not seen, so this is a lower
                bound.
mark rate       marked objects over time in the mark+trace phases.
sweep rate      objects visited by the sweep over time in the sweep phase.

A figure the source cannot supply (the JVM log has no phases) prints as n/a.
"""

import argparse
import bisect
import json
import sys

PHASES = ("mark", "trace", "strings", "sweep")
DEFAULT_MMU_MS = (1, 10, 100, 1000)


VERSION = 3
GC_REQUIRED = ("cause", "t0", "t4", "bytes_before", "bytes_after")
CYCLE_FIELDS = (
    "cause", "t0", "t1", "steps", "bytes_before", "bytes_after",
    "objs_before", "objs_after", "marked", "freed",
)
STEP_KINDS = ("start", "mark", "finish", "sweep", "end")
STALL_KINDS = ("new", "free")
V3_RECORDS = ("cycle", "step", "stall")


def parse_records(path):
    """Read a trace file. Returns a dict with start, end, gcs, cycles, steps
    and stalls. A malformed record raises ValueError with the file and line."""
    start = end = None
    version = 1
    gcs, cycles, steps, stalls = [], [], [], []

    def bad(n, msg):
        raise ValueError(f"{path}:{n}: {msg}")

    def ints(n, kv, keys, name):
        for k in keys:
            if k not in kv:
                bad(n, f"{name} record has no {k}")
            try:
                kv[k] = int(kv[k])
            except ValueError:
                bad(n, f"{name} field {k}={kv[k]!r} is not an integer")
            if kv[k] < 0:
                bad(n, f"{name} field {k} is negative")

    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            if line.startswith("#"):
                words = line.split()
                if len(words) == 3 and words[1] == "loxpp-gc-trace":
                    if not (words[2].startswith("v") and words[2][1:].isdigit()):
                        bad(n, f"bad version '{words[2]}'")
                    version = int(words[2][1:])
                    if version > VERSION:
                        bad(n, f"trace version {version} is newer than {VERSION}")
                continue
            name, *fields = line.split()
            kv = {}
            for x in fields:
                if "=" not in x:
                    bad(n, f"field '{x}' is not key=value")
                k, v = x.split("=", 1)
                kv[k] = v
            if name in V3_RECORDS and version < 3:
                bad(n, f"{name} record in a version {version} trace")
            if name == "start":
                ints(n, kv, ("t",), name)
                start = kv["t"]
            elif name == "end":
                ints(n, kv, ("t",), name)
                end = kv["t"]
            elif name == "gc":
                for k in GC_REQUIRED:
                    if k not in kv:
                        bad(n, f"gc record has no {k}")
                ints(n, kv, [k for k in kv if k != "cause"], name)
                gcs.append(kv)
            elif name == "cycle":
                ints(n, kv, [k for k in CYCLE_FIELDS if k != "cause"], name)
                if "cause" not in kv:
                    bad(n, "cycle record has no cause")
                if kv["t1"] < kv["t0"]:
                    bad(n, "cycle ends before it starts")
                cycles.append(kv)
            elif name == "step":
                ints(n, kv, ("cycle", "t0", "t1", "units"), name)
                if kv.get("kind") not in STEP_KINDS:
                    bad(n, f"step kind {kv.get('kind')!r} is not one of "
                           f"{', '.join(STEP_KINDS)}")
                if kv["t1"] < kv["t0"]:
                    bad(n, "step ends before it starts")
                steps.append(kv)
            elif name == "stall":
                ints(n, kv, ("t0", "t1"), name)
                if kv.get("kind") not in STALL_KINDS:
                    bad(n, f"stall kind {kv.get('kind')!r} is not one of "
                           f"{', '.join(STALL_KINDS)}")
                if kv["t1"] < kv["t0"]:
                    bad(n, "stall ends before it starts")
                stalls.append(kv)
            else:
                bad(n, f"unknown record '{name}'")
    if start is None:
        raise ValueError(f"{path}: no start record")
    if end is None:
        last = [g["t4"] for g in gcs] + [c["t1"] for c in cycles] + [
            x["t1"] for x in steps]
        end = max(last) if last else start
    return {"start": start, "end": end, "version": version, "gcs": gcs,
            "cycles": cycles, "steps": steps, "stalls": stalls}


def parse_trace(path):
    """The v2 view of a trace: (start, end, gc records)."""
    t = parse_records(path)
    return t["start"], t["end"], t["gcs"]


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
    t = parse_records(path)
    return summarise_gcs(path, t["start"], t["end"], t["gcs"], mmu_ms,
                         cycles=t["cycles"], steps=t["steps"],
                         stalls=t["stalls"])


def merge_intervals(intervals):
    """Sorted, non-overlapping intervals; those that touch become one."""
    out = []
    for b, e in sorted(intervals):
        if out and b <= out[-1][1]:
            out[-1] = (out[-1][0], max(out[-1][1], e))
        else:
            out.append((b, e))
    return out


def summarise_gcs(label, start, end, gcs, mmu_ms, cycles=(), steps=(), stalls=()):
    """Compute the report from collection records, whatever produced them.

    A gc record needs t0, t4 (pause begin and end, ns), bytes_before,
    bytes_after and cause. The phase timestamps t1..t3 and the counts
    marked, objs_before, freed are optional: when a source does not have
    them for every record, the figures that need them are None and render
    as n/a. tools/jvm_gc_log.py builds records from a JVM -Xlog:gc log.

    cycles, steps and stalls are the records of an incremental collector
    (trace format 3). A step is a pause. A cycle is a summary and counts as
    a collection. A stall is not a pause; it has its own figures.
    """
    wall = max(end - start, 1)
    pause_iv = [(g["t0"], g["t4"]) for g in gcs] + [
        (x["t0"], x["t1"]) for x in steps
    ]
    pauses_ns = [b - a for a, b in pause_iv]
    sorted_p = sorted(pauses_ns)
    total_pause = sum(b - a for a, b in merge_intervals(pause_iv))

    def has(*keys):
        return all(k in g for g in gcs for k in keys)

    # Phase shares and rates come from stop-the-world records, which carry
    # the phase times. A trace with cycles has none for the cycle part.
    gc_pause_total = sum(g["t4"] - g["t0"] for g in gcs)
    phase_ns = None
    if gcs and has("t1", "t2", "t3"):
        phase_ns = {
            "mark": sum(g["t1"] - g["t0"] for g in gcs),
            "trace": sum(g["t2"] - g["t1"] for g in gcs),
            "strings": sum(g["t3"] - g["t2"] for g in gcs),
            "sweep": sum(g["t4"] - g["t3"] for g in gcs),
        }
    elif not gcs and not cycles:
        phase_ns = None
    allocated = 0
    prev_after = 0
    all_collections = sorted(
        list(gcs) + list(cycles),
        key=lambda g: g["t0"],
    )
    for g in all_collections:
        allocated += max(0, g["bytes_before"] - prev_after)
        prev_after = g["bytes_after"]
    causes = {}
    for g in all_collections:
        causes[g["cause"]] = causes.get(g["cause"], 0) + 1
    mark_rate = sweep_rate = None
    if phase_ns and has("marked"):
        marked = sum(g["marked"] for g in gcs)
        mark_rate = rate(marked, phase_ns["mark"] + phase_ns["trace"]) / 1e6
    if phase_ns and has("objs_before"):
        visited = sum(g["objs_before"] for g in gcs)
        sweep_rate = rate(visited, phase_ns["sweep"]) / 1e6

    stall_iv = [(x["t0"], x["t1"]) for x in stalls]
    stall_info = {}
    for kind in STALL_KINDS:
        ds = [x["t1"] - x["t0"] for x in stalls if x["kind"] == kind]
        if ds:
            stall_info[kind] = {"count": len(ds), "max_us": max(ds) / 1e3}
    merged = merge_intervals(pause_iv)
    merged_all = merge_intervals(pause_iv + stall_iv)
    freed = None
    if all_collections and all("freed" in g for g in all_collections):
        freed = sum(g["freed"] for g in all_collections)
    return {
        "file": label,
        "collections": len(all_collections),
        "causes": causes,
        "wall_ms": wall / 1e6,
        "gc_ms": total_pause / 1e6,
        "gc_overhead_pct": 100.0 * total_pause / wall,
        "pause_p50_us": percentile(sorted_p, 50) / 1e3,
        "pause_p99_us": percentile(sorted_p, 99) / 1e3,
        "pause_p999_us": percentile(sorted_p, 99.9) / 1e3,
        "pause_p9999_us": percentile(sorted_p, 99.99) / 1e3,
        "pause_max_us": (sorted_p[-1] if sorted_p else 0) / 1e3,
        "steps": len(steps),
        "max_step_units": max((x["units"] for x in steps), default=None),
        "stalls": stall_info,
        "phase_pct": None
        if phase_ns is None
        else {
            k: (100.0 * v / gc_pause_total if gc_pause_total else 0.0)
            for k, v in phase_ns.items()
        },
        "mmu": {str(ms): mmu(start, end, merged, ms * 1_000_000) for ms in mmu_ms},
        "mmu_with_stalls": {
            str(ms): mmu(start, end, merged_all, ms * 1_000_000) for ms in mmu_ms
        }
        if stalls
        else None,
        "alloc_mb_per_s": rate(allocated, wall) / 1e6,
        "mark_mobj_per_s": mark_rate,
        "sweep_mobj_per_s": sweep_rate,
        "objects_freed": freed,
    }


def render(s):
    def num(v, fmt):
        return "n/a" if v is None else f"{v:{fmt}}"

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
        f"pause p99.9/p99.99 {s['pause_p999_us']:.1f} / {s['pause_p9999_us']:.1f} us"
    )
    if s["steps"]:
        out.append(f"steps             {s['steps']}  max units {s['max_step_units']}")
    if s["phase_pct"] is None:
        out.append("phase share       n/a")
    else:
        out.append(
            "phase share       "
            + "  ".join(f"{k} {s['phase_pct'][k]:.1f}%" for k in PHASES)
        )
    mm = "  ".join(
        f"{ms}ms:{'n/a' if v is None else f'{100 * v:.1f}%'}"
        for ms, v in s["mmu"].items()
    )
    out.append(f"MMU               {mm}")
    if s["mmu_with_stalls"]:
        mw = "  ".join(
            f"{ms}ms:{'n/a' if v is None else f'{100 * v:.1f}%'}"
            for ms, v in s["mmu_with_stalls"].items()
        )
        out.append(f"MMU with stalls   {mw}")
    for kind, info in sorted(s["stalls"].items()):
        out.append(
            f"stall {kind:<4}        {info['count']} over the limit, "
            f"max {info['max_us']:.1f} us"
        )
    out.append(f"alloc rate        {s['alloc_mb_per_s']:.1f} MB/s")
    out.append(f"mark rate         {num(s['mark_mobj_per_s'], '.2f')} Mobj/s")
    out.append(f"sweep rate        {num(s['sweep_mobj_per_s'], '.2f')} Mobj/s")
    out.extend(s.get("notes", []))
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
