#!/usr/bin/env python3
"""Print the native and the JVM GC report for each GC-bound benchmark.

Both reports have the format of tools/gc_report.py. The native run writes a
LOXPP_GC_TRACE file; the JVM run writes an -Xlog:gc* log that
tools/jvm_gc_log.py converts. Semantic differences between the two are in
tools/jvm_gc_log.py and in benchmarks/README.md.

Usage:
  python3 benchmarks/gc_compare.py                  # the four GC-bound programs
  python3 benchmarks/gc_compare.py --latency        # ... and latency/*.lox
  python3 benchmarks/gc_compare.py --only storage --jvm-opts=-XX:+UseG1GC
  python3 benchmarks/gc_compare.py --json results/gc.json

Needs a release build/loxpp, runtime/jvm/lox-rt.jar, and generated programs
(see benchmarks/README.md). Each process is pinned to one cpu as in run.py, so
the JVM picks the Serial collector unless --jvm-opts selects another.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(HERE))
import gc_report  # noqa: E402
import jvm_gc_log  # noqa: E402
import run  # noqa: E402

GC_BOUND = ["storage", "binary_trees", "towers", "json"]


def native_report(prog: Path, workdir: Path, timeout: int, mmu_ms):
    trace = workdir / f"{prog.stem}.native.trace"
    cmd = run.PIN + run.BACKENDS["native"](str(prog))
    env = {**os.environ, "LOXPP_GC_TRACE": str(trace)}
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
    if p.returncode != 0:
        raise RuntimeError(f"native exit {p.returncode}: {p.stderr.strip()[:200]}")
    if not trace.exists():
        raise RuntimeError("native run wrote no GC trace")
    s = gc_report.summarise(str(trace), mmu_ms)
    s["file"] = f"{prog.stem} [native]"
    return s


def jvm_report(prog: Path, workdir: Path, timeout: int, mmu_ms, jvm_opts: str):
    log = workdir / f"{prog.stem}.jvm.log"
    opts = f"{jvm_opts} -Xlog:gc*:file={log}:uptimenanos".strip()
    cmd = run.PIN + run.BACKENDS["jvm"](str(prog))
    env = {**os.environ, "LOXPP_JVM_OPTS": opts}
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
    if p.returncode != 0:
        raise RuntimeError(f"jvm exit {p.returncode}: {p.stderr.strip()[:200]}")
    s = jvm_gc_log.summarise(str(log), mmu_ms)
    s["file"] = s["file"].replace(str(log), f"{prog.stem}")
    return s


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--only", nargs="+", help="program names from programs/ or latency/")
    ap.add_argument("--latency", action="store_true",
                    help="also run the programs in latency/")
    ap.add_argument("--jvm-opts", default="", help="extra JVM flags, whitespace-split")
    ap.add_argument("--mmu-windows", default="1,10,100,1000")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--json", help="write both reports for every program here")
    args = ap.parse_args()
    mmu_ms = [float(x) if "." in x else int(x) for x in args.mmu_windows.split(",")]

    progs = [run.PROGRAMS / f"{n}.lox" for n in (args.only or GC_BOUND)
             if (run.PROGRAMS / f"{n}.lox").exists()]
    if args.only:
        progs += [p for p in sorted(run.LATENCY.glob("*.lox")) if p.stem in args.only]
    elif args.latency:
        progs += sorted(run.LATENCY.glob("*.lox"))
    missing = [n for n in (args.only or []) if n not in {p.stem for p in progs}]
    if missing:
        print(f"no such program: {', '.join(missing)}", file=sys.stderr)
        return 2
    if not progs:
        print("no programs found; run benchmarks/generate.py first", file=sys.stderr)
        return 2

    results, failed = [], False
    with tempfile.TemporaryDirectory() as tmp:
        for prog in progs:
            entry = {"program": prog.stem}
            for kind, fn in (("native", lambda: native_report(prog, Path(tmp), args.timeout, mmu_ms)),
                             ("jvm", lambda: jvm_report(prog, Path(tmp), args.timeout, mmu_ms, args.jvm_opts))):
                try:
                    entry[kind] = fn()
                    print(gc_report.render(entry[kind]) + "\n")
                except (RuntimeError, OSError, subprocess.TimeoutExpired, ValueError) as e:
                    entry[kind] = {"error": str(e)}
                    print(f"== {prog.stem} [{kind}]\nFAIL: {e}\n")
                    failed = True
            results.append(entry)
    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=2))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
