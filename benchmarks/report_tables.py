#!/usr/bin/env python3
"""Render the markdown table blocks for notes/benchmark_report_*.md from the
run.py and profile.py JSON output.

  python3 benchmarks/report_tables.py results/run.json results/profile.json
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

COMPUTE = {"fib", "mandelbrot", "nbody", "spectral_norm", "sieve", "fasta",
           "reverse_complement", "permute"}
ALLOC = {"bounce", "storage", "list", "towers", "binary_trees", "json",
         "richards", "k_nucleotide", "queens"}


def ms(us):
    return f"{us/1000:.1f}" if us is not None else "—"


def main() -> None:
    run = json.loads(Path(sys.argv[1]).read_text())
    prof = json.loads(Path(sys.argv[2]).read_text()) if len(sys.argv) > 2 else None

    def cell(e, b, key="steady_us"):
        r = e["backends"].get(b, {})
        return r.get(key) if r.get("ok") else None

    print("### RESULTS_TABLE\n")
    print("| benchmark | native ms | jvm ms | jvm/nat | spread nat/jvm |")
    print("|---|--:|--:|--:|---|")
    geo = {"jvm": []}
    for e in sorted(run, key=lambda e: e["program"]):
        n = cell(e, "native"); j = cell(e, "jvm")
        jr = f"{j/n:.2f}" if (n and j) else "—"
        if n and j:
            geo["jvm"].append(j / n)
        sp = "/".join(
            f"{e['backends'].get(b, {}).get('spread', 0)*100:.0f}%"
            for b in ("native", "jvm"))
        mark = "" if e["checksum_match"] else " ⚠"
        print(f"| {e['program']}{mark} | {ms(n)} | {ms(j)} | {jr} | {sp} |")

    def geomean(xs):
        p = 1.0
        for x in xs:
            p *= x
        return p ** (1 / len(xs)) if xs else float("nan")

    print(f"\n_geomean: jvm {geomean(geo['jvm']):.2f}× native "
          f"(< 1 = faster than native)_\n")

    print("### PEAK_TABLE\n")
    print("| benchmark | native | jvm |")
    print("|---|--:|--:|")
    for e in sorted(run, key=lambda e: e["program"]):
        print(f"| {e['program']} | {ms(cell(e,'native','min_us'))} "
              f"| {ms(cell(e,'jvm','min_us'))} |")

    print("\n### OVERHEAD_TABLE\n")
    print("| benchmark | native | jvm |")
    print("|---|--:|--:|")
    for e in sorted(run, key=lambda e: e["program"]):
        row = []
        for b in ("native", "jvm"):
            r = e["backends"].get(b, {})
            row.append(f"{r['overhead_s']:.2f}s" if r.get("ok") else "—")
        print(f"| {e['program']} | {row[0]} | {row[1]} |")

    if prof:
        print("\n### OPCODE_TABLE\n")
        print("| opcode | count | share |")
        print("|---|--:|--:|")
        agg = prof["aggregate"]
        tot = sum(agg.values())
        for op, c in list(agg.items())[:16]:
            print(f"| `{op}` | {c:,} | {100*c/tot:.1f}% |")

        print("\n### PROFILE_PER_PROGRAM\n")
        print("| program | total opcodes | GC runs | GC ms | top opcodes |")
        print("|---|--:|--:|--:|---|")
        for p in prof["per_program"]:
            top = ", ".join(f"{o['op']} {o['pct']:.0f}%" for o in p["opcodes"][:4])
            print(f"| {p['program']} | {p['total_ops']:,} | {p['gc']['collections']} "
                  f"| {p['gc']['total_ms']:.1f} | {top} |")


if __name__ == "__main__":
    main()
