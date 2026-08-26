#!/usr/bin/env python3
"""Run the generated benchmarks on the native / JVM / CLR backends.

Each program prints  HARNESS <batch> <us> <checksum>  lines (see generate.py).
This runner launches every (program, backend) pair PROCS times, pinned to one
CPU, and reports:

  steady_us   median over the measured batches of the fastest process launch
              (per-batch CPU time from the program's own clock())
  spread      (max-min)/median of those measured batches, a noise indicator
  wall_s      external wall-clock of the whole process (median launch)
  overhead_s  wall_s minus the program's own total measured+warmup time:
              parse + (JVM/CLR) assemble + runtime startup + JIT warm-up

Checksums from all three backends must agree, or the row is flagged MISMATCH.

Usage:
  python3 benchmarks/run.py                      # all programs, all backends
  python3 benchmarks/run.py --only fib mandelbrot
  python3 benchmarks/run.py --backends native
  python3 benchmarks/run.py --procs 5 --json results/run.json
"""
from __future__ import annotations

import argparse
import json
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).parent
ROOT = HERE.parent
PROGRAMS = HERE / "programs"

HARNESS_RE = re.compile(r"^HARNESS\s+(\d+)\s+([\d.eE+-]+)\s+(.*)$")

BACKENDS = {
    "native": lambda prog: [str(ROOT / "build" / "loxpp"), prog],
    "jvm":    lambda prog: [str(ROOT / "tools" / "loxpp_jvm.sh"), prog],
    "clr":    lambda prog: [str(ROOT / "tools" / "loxpp_clr.sh"), prog],
}

PIN = ["taskset", "-c", "0"]


def parse_harness(stdout: str):
    warm, meas, checksum = [], [], None
    # generate.py: warmup batches print nothing; only measured batches print.
    for line in stdout.splitlines():
        m = HARNESS_RE.match(line.strip())
        if m:
            meas.append(float(m.group(2)))
            checksum = m.group(3).strip()
    return warm, meas, checksum


def one_launch(cmd: list[str], timeout: int):
    t0 = time.monotonic()
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    wall = time.monotonic() - t0
    return p.returncode, p.stdout, p.stderr, wall


def run_pair(name: str, backend: str, procs: int, timeout: int):
    prog = str(PROGRAMS / f"{name}.lox")
    cmd = PIN + BACKENDS[backend](prog)
    launches = []
    err = None
    for _ in range(procs):
        try:
            rc, out, serr, wall = one_launch(cmd, timeout)
        except subprocess.TimeoutExpired:
            err = f"timeout>{timeout}s"
            break
        if rc != 0:
            err = f"exit {rc}: {serr.strip()[:200]}"
            break
        _, meas, checksum = parse_harness(out)
        if not meas:
            err = f"no HARNESS lines; stderr={serr.strip()[:160]}"
            break
        # drop the first third of the measured batches: HotSpot tiered
        # compilation is still improving some benchmarks past the warm-up.
        trimmed = meas[len(meas) // 3:] or meas
        launches.append({"wall": wall, "meas": meas, "trim": trimmed,
                         "checksum": checksum})
    if err:
        return {"backend": backend, "ok": False, "error": err}
    best = min(launches, key=lambda L: statistics.median(L["trim"]))
    tm = best["trim"]
    med = statistics.median(tm)
    walls = sorted(L["wall"] for L in launches)
    wall_med = walls[len(walls) // 2]
    prog_total_s = sum(best["meas"]) / 1e6
    return {
        "backend": backend,
        "ok": True,
        "checksum": best["checksum"],
        "steady_us": med,
        "min_us": min(min(L["trim"]) for L in launches),
        "spread": (max(tm) - min(tm)) / med if med else 0.0,
        "wall_s": wall_med,
        "overhead_s": max(0.0, wall_med - prog_total_s),
        "all_median_us": [round(statistics.median(L["trim"]), 1) for L in launches],
        "batches_us": [round(x, 1) for x in best["meas"]],
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="+", help="subset of program names")
    ap.add_argument("--backends", nargs="+", default=list(BACKENDS),
                    choices=list(BACKENDS))
    ap.add_argument("--procs", type=int, default=3)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--json", help="write full results here")
    args = ap.parse_args()

    names = sorted(p.stem for p in PROGRAMS.glob("*.lox"))
    if args.only:
        names = [n for n in names if n in args.only]

    rows = []
    for name in names:
        entry = {"program": name, "backends": {}}
        cs = {}
        for backend in args.backends:
            print(f"  {name:20s} [{backend}] ...", end=" ", flush=True)
            r = run_pair(name, backend, args.procs, args.timeout)
            entry["backends"][backend] = r
            if r["ok"]:
                cs[backend] = r["checksum"]
                print(f"steady={r['steady_us']/1000:.1f}ms "
                      f"wall={r['wall_s']:.2f}s spread={r['spread']*100:.0f}%")
            else:
                print(f"FAIL: {r['error']}")
        entry["checksum_match"] = len(set(cs.values())) <= 1
        if not entry["checksum_match"]:
            print(f"  !! {name}: checksum MISMATCH {cs}")
        rows.append(entry)

    print()
    _table(rows, args.backends)
    if args.json:
        Path(args.json).write_text(json.dumps(rows, indent=2))
        print(f"\nwrote {args.json}")


def _table(rows, backends):
    w = 20
    hdr = f"{'program':<{w}}" + "".join(f"{b+' ms':>14}" for b in backends) \
        + f"{'jvm/nat':>10}{'clr/nat':>10}  match"
    print(hdr)
    print("-" * len(hdr))
    for e in rows:
        line = f"{e['program']:<{w}}"
        vals = {}
        for b in backends:
            r = e["backends"][b]
            if r["ok"]:
                vals[b] = r["steady_us"] / 1000.0
                line += f"{vals[b]:>14.1f}"
            else:
                line += f"{'FAIL':>14}"
        nat = vals.get("native")
        for b in ("jvm", "clr"):
            if nat and b in vals and nat > 0:
                line += f"{vals[b]/nat:>10.2f}"
            else:
                line += f"{'-':>10}"
        line += f"   {'ok' if e['checksum_match'] else 'MISMATCH'}"
        print(line)


if __name__ == "__main__":
    main()
