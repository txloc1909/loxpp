#!/usr/bin/env python3
"""Apples-to-apples cross-language microbenchmarks.

Every language runs the IDENTICAL algorithm at the IDENTICAL problem size and
self-times only the compute region (so interpreter/container startup is
excluded), printing the computed result and then the elapsed seconds. We run
each program N times and keep the minimum (least interference).

lox++ and clox share the strict book-Lox programs in benchmarks/clox/; the
python/lua/js ports in benchmarks/<lang>/ reproduce the same algorithms. This
is the only honest way to compare languages — see runner.py for the
per-language AWFY/CLBG/Wren suites, which are NOT cross-comparable.

Requires the images: loxpp-dev-env, bench-clox, bench-lua (see
containers/Dockerfile.*) and pulls python:3.12-slim / node:22-alpine.
"""
import argparse
import math
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BENCHES = ["fib", "towers", "queens", "mandelbrot"]


def _podman(*args):
    return ["podman", "run", "--rm", *args]


# Each entry maps a bench name to the argv that runs that language's port.
LANGS = {
    "lox": lambda b: _podman(
        "-v", f"{ROOT}:/workspace:ro,z", "loxpp-dev-env",
        "/workspace/build/loxpp", f"/workspace/benchmarks/clox/{b}.lox"),
    "clox": lambda b: _podman("bench-clox", "clox", f"/benchmarks/clox/{b}.lox"),
    "python": lambda b: _podman(
        "-v", f"{ROOT}/benchmarks/python:/bench:ro,z", "python:3.12-slim",
        "python3", f"/bench/{b}.py"),
    "lua": lambda b: _podman(
        "-v", f"{ROOT}/benchmarks/lua:/bench:ro,z", "bench-lua",
        "lua5.4", f"/bench/{b}.lua"),
    "js": lambda b: _podman(
        "-v", f"{ROOT}/benchmarks/js:/bench:ro,z", "node:22-alpine",
        "node", f"/bench/{b}.js"),
    "wren": lambda b: _podman(
        "-v", f"{ROOT}/benchmarks/wren:/bench:ro,z", "bench-wren",
        "wren", f"/bench/{b}.wren"),
}

_NUM = re.compile(r"^\s*([-\d.eE+]+)\s*$")


def run_once(lang, bench):
    """Return (result_str, elapsed_seconds). Last numeric line is the time,
    the line before it is the computed result (for a correctness check)."""
    out = subprocess.run(LANGS[lang](bench), capture_output=True, text=True,
                         timeout=300)
    nums = [m.group(1) for ln in out.stdout.splitlines()
            if (m := _NUM.match(ln))]
    if len(nums) < 2:
        raise RuntimeError(f"{lang}/{bench}: unparseable output: {out.stdout!r} {out.stderr!r}")
    return nums[-2], float(nums[-1])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--lang", nargs="+", default=list(LANGS))
    ap.add_argument("--bench", nargs="+", default=BENCHES)
    args = ap.parse_args()

    rows = []
    for b in args.bench:
        times, result = {}, None
        for lang in args.lang:
            best, res = None, None
            for _ in range(args.reps):
                res, t = run_once(lang, b)
                best = t if best is None else min(best, t)
            times[lang] = best
            if result is None:
                result = res
            elif not math.isclose(float(res), float(result), rel_tol=1e-4):
                # lox/clox str() prints large ints in lossy scientific notation
                # (9.22746e+06), so compare numerically with a tolerance.
                print(f"WARNING: {b}/{lang} result {res} != {result}")
        rows.append((b, result, times))

    # Markdown table of absolute times (ms) and ×fastest.
    print("| benchmark | result | " + " | ".join(args.lang) + " |")
    print("|" + "---|" * (len(args.lang) + 2))
    for b, result, times in rows:
        fastest = min(times.values())
        cells = []
        for lang in args.lang:
            t = times[lang]
            cells.append(f"{t*1000:.2f}ms ({t/fastest:.1f}×)")
        print(f"| {b} | {result} | " + " | ".join(cells) + " |")


if __name__ == "__main__":
    main()
