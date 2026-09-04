#!/usr/bin/env python3
"""Run each benchmark's profiling variant on the LOXPP_PROFILE native build and
collect the opcode-dispatch histogram, function self-time, and GC stats.

Needs a build configured with -DLOXPP_PROFILE=ON at  ../build-profile/loxpp .
The profiler writes its report to stderr (src/profiler.h).

Usage:
  python3 benchmarks/profile.py --json results/profile.json
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path

HERE = Path(__file__).parent
ROOT = HERE.parent
PROF_PROGRAMS = HERE / "programs" / "prof"
PROF_BIN = ROOT / "build-profile" / "loxpp"

OP_RE = re.compile(r"^\s+([A-Z_]+)\s+(\d+)\s+([\d.]+)\s*%$")
GC_RE = re.compile(r"GC pauses\s*:\s*(\d+) collections,\s*([\d.]+) ms total,\s*([\d.]+) ms max")
CPU_RE = re.compile(r"Total CPU time\s*:\s*([\d.]+) ms")
FN_RE = re.compile(r"^\s+(\S+)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s*%$")


def profile_one(name: str, timeout: int = 600) -> dict:
    prog = str(PROF_PROGRAMS / f"{name}.lox")
    p = subprocess.run([str(PROF_BIN), prog], capture_output=True, text=True,
                       timeout=timeout)
    if p.returncode != 0:
        return {
            "program": name,
            "ok": False,
            "error": f"exit {p.returncode}: {p.stderr.strip()[:200]}",
            "exit": p.returncode,
        }
    rep = p.stderr
    ops, fns = [], []
    in_ops = in_fns = False
    for line in rep.splitlines():
        if "Opcode Dispatch Counts" in line:
            in_ops, in_fns = True, False
            continue
        if "Function Call Profile" in line:
            in_ops, in_fns = False, True
            continue
        if not line.strip() or line.lstrip().startswith("==="):
            in_ops = in_fns = False
        if in_ops:
            m = OP_RE.match(line)
            if m:
                ops.append((m.group(1), int(m.group(2)), float(m.group(3))))
        elif in_fns:
            m = FN_RE.match(line)
            if m:
                fns.append({
                    "fn": m.group(1), "calls": int(m.group(2)),
                    "total_ms": float(m.group(3)), "self_ms": float(m.group(4)),
                    "self_pct": float(m.group(5)),
                })
    gc = GC_RE.search(rep)
    cpu = CPU_RE.search(rep)
    total_ops = sum(c for _, c, _ in ops)
    return {
        "program": name,
        "ok": True,
        "total_ops": total_ops,
        "opcodes": [{"op": o, "count": c, "pct": p} for o, c, p in ops],
        "functions": sorted(fns, key=lambda f: -f["self_ms"])[:12],
        "gc": {
            "collections": int(gc.group(1)) if gc else 0,
            "total_ms": float(gc.group(2)) if gc else 0.0,
            "max_ms": float(gc.group(3)) if gc else 0.0,
        },
        "profiled_cpu_ms": float(cpu.group(1)) if cpu else None,
        "exit": p.returncode,
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="+")
    ap.add_argument("--json")
    args = ap.parse_args()
    names = sorted(p.stem for p in PROF_PROGRAMS.glob("*.lox"))
    if args.only:
        names = [n for n in names if n in args.only]

    out = []
    # aggregate opcode counts weighted equally across programs
    agg: dict[str, int] = {}
    for name in names:
        print(f"  profiling {name} ...", end=" ", flush=True)
        r = profile_one(name)
        out.append(r)
        if not r["ok"]:
            print(f"FAIL: {r['error']}")
            continue
        for o in r["opcodes"]:
            agg[o["op"]] = agg.get(o["op"], 0) + o["count"]
        top = ", ".join(f"{o['op']}={o['pct']:.0f}%" for o in r["opcodes"][:4])
        print(f"{r['total_ops']:,} ops  gc={r['gc']['collections']}  [{top}]")

    grand = sum(agg.values())
    print("\n=== aggregate opcode share across all benchmarks ===")
    for op, c in sorted(agg.items(), key=lambda kv: -kv[1]):
        print(f"  {op:20s} {c:14,d}  {100*c/grand:5.1f}%")

    if args.json:
        Path(args.json).write_text(json.dumps(
            {"per_program": out,
             "aggregate": {op: c for op, c in sorted(agg.items(), key=lambda kv: -kv[1])}},
            indent=2))
        print(f"\nwrote {args.json}")


if __name__ == "__main__":
    main()
