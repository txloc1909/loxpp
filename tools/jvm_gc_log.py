#!/usr/bin/env python3
"""Summarise a JVM unified GC log in the format of tools/gc_report.py.

Usage: jvm_gc_log.py [--json] [--mmu-windows MS,MS,...] LOG [LOG...]

Producing a log (uptime in nanoseconds is required; the parser rejects a log
without it):

    LOXPP_JVM_OPTS="-Xlog:gc*:file=/tmp/gc.log:uptimenanos" \\
        tools/loxpp_jvm.sh program.lox

What becomes a collection. Every stop-the-world pause the log reports as
"GC(n) Pause <kind> ... <before>-><after>(<committed>) <ms>" is one record:
young, full, remark, and cleanup pauses for G1; young and full for Serial and
Parallel. The pause ends at the log line's timestamp and began <ms> earlier.
These records go through gc_report.summarise_gcs, so overhead, percentiles,
MMU, and allocation rate use the same code as the native report.

Differences from the native report:
  * Only stop-the-world time counts as a pause. G1 concurrent cycles run on
    other threads while the program runs; they are not in gc time, pauses, or
    MMU. Their count and total duration are in a "concurrent" line instead.
  * Heap sizes in the log are rounded (to whole MB or KB), so the allocation
    rate is approximate. It also counts bytes the JIT and class loading
    allocate, which the native count has no equivalent for.
  * The log has no per-phase timestamps and no object counts, so phase share,
    mark rate, and sweep rate are n/a.
  * wall is JVM uptime at the last log line, from JVM start. Jasmin assembly
    before the JVM starts is not in it. The native wall starts when the
    memory manager is created.
  * The collector is chosen by the JVM. On a host or cpu set with fewer than
    two cpus it is Serial, not G1; the collector name is in the first line.
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gc_report  # noqa: E402

LINE_RE = re.compile(r"^\[(\d+)ns\] (.*)$")
PAUSE_RE = re.compile(
    r"^GC\(\d+\) Pause (\w+).*? (\d+)([KMG])->(\d+)([KMG])\(\d+[KMG]\) ([\d.]+)ms$"
)
CONCURRENT_RE = re.compile(r"^GC\(\d+\) Concurrent (Mark|Undo) Cycle ([\d.]+)ms$")
COLLECTOR_RE = re.compile(r"^Using (.+)$")
UNIT = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30}


def parse_log(text):
    """Return (collector, start_ns, end_ns, gcs, concurrent) for a log."""
    collector = None
    last = None
    gcs = []
    concurrent = []
    for n, raw in enumerate(text.splitlines(), 1):
        if not raw.strip():
            continue
        m = LINE_RE.match(raw)
        if not m:
            raise ValueError(f"line {n}: no [<ns>ns] uptime decoration: {raw!r}")
        t = int(m.group(1))
        last = t
        body = m.group(2)
        c = COLLECTOR_RE.match(body)
        if c and collector is None:
            collector = c.group(1)
            continue
        p = PAUSE_RE.match(body)
        if p:
            kind, b, bu, a, au, ms = p.groups()
            t4 = t
            t0 = t4 - round(float(ms) * 1e6)
            gcs.append(
                {
                    "cause": kind.lower(),
                    "t0": t0,
                    "t4": t4,
                    "bytes_before": int(b) * UNIT[bu],
                    "bytes_after": int(a) * UNIT[au],
                }
            )
            continue
        k = CONCURRENT_RE.match(body)
        if k:
            concurrent.append(float(k.group(2)))
    if last is None:
        raise ValueError("empty log")
    return collector or "unknown", 0, last, gcs, concurrent


def summarise(path, mmu_ms):
    with open(path) as f:
        collector, start, end, gcs, concurrent = parse_log(f.read())
    s = gc_report.summarise_gcs(f"{path} [jvm {collector}]", start, end, gcs, mmu_ms)
    s["notes"] = [
        f"concurrent        {len(concurrent)} cycles, {sum(concurrent):.2f} ms"
        " (not counted as pause)"
    ]
    s["concurrent_cycles"] = len(concurrent)
    s["concurrent_ms"] = sum(concurrent)
    return s


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--json", action="store_true", help="emit one JSON list")
    ap.add_argument(
        "--mmu-windows",
        default=",".join(map(str, gc_report.DEFAULT_MMU_MS)),
        help="comma-separated MMU window sizes in ms",
    )
    args = ap.parse_args(argv)
    mmu_ms = [float(x) if "." in x else int(x) for x in args.mmu_windows.split(",")]
    results = [summarise(p, mmu_ms) for p in args.logs]
    if args.json:
        import json

        json.dump(results, sys.stdout, indent=2)
        print()
    else:
        print("\n\n".join(gc_report.render(r) for r in results))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
