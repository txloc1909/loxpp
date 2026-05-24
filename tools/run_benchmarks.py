#!/usr/bin/env python3
"""Lox++ benchmark runner.

Runs each .lox file in the benchmarks/ directory, collects timing, and
prints a summary table.

Usage:
  python3 tools/run_benchmarks.py [options]

Options:
  --interpreter PATH   Path to the Lox++ binary (default: build/loxpp)
  --filter PATTERN     Only run benchmarks whose name contains PATTERN
  --iterations N       Number of timed runs per benchmark (default: 5)
  --json FILE          Write raw results to FILE as JSON
"""

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCHMARKS_DIR = os.path.join(REPO_ROOT, "benchmarks")

# Expected results for correctness verification. Key = benchmark stem.
EXPECTED_RESULTS = {
    "fib":          "55",      # fib(10) = 55, stable checksum
    "binary_trees": "true",    # long-lived tree check > 0
    "nbody":        "true",    # energy < -0.1
    "mandelbrot":   "true",    # nonzero pixel count
    "method_call":  "true",    # toggle.value() after even number of flips
    "sieve":        "148933",  # primes up to 2000000
    "towers":       "true",    # move_count == 2^20 - 1
}


def parse_args():
    p = argparse.ArgumentParser(description="Lox++ benchmark runner")
    p.add_argument("--interpreter", default=os.path.join(REPO_ROOT, "build", "loxpp"),
                   help="Path to loxpp binary (default: build/loxpp)")
    p.add_argument("--filter", default="", metavar="PATTERN",
                   help="Only run benchmarks whose name contains PATTERN")
    p.add_argument("--iterations", type=int, default=5, metavar="N",
                   help="Timed runs per benchmark (default: 5)")
    p.add_argument("--json", metavar="FILE",
                   help="Write raw results to FILE as JSON")
    return p.parse_args()


def discover_benchmarks(filter_str):
    files = sorted(
        f for f in os.listdir(BENCHMARKS_DIR)
        if f.endswith(".lox") and filter_str in f
    )
    return [os.path.join(BENCHMARKS_DIR, f) for f in files]


def run_once(interpreter, benchmark_path):
    """Run one benchmark and return (elapsed_s, result_str) or raise."""
    wall_start = time.perf_counter()
    try:
        proc = subprocess.run(
            [interpreter, benchmark_path],
            capture_output=True,
            text=True,
            timeout=120,
        )
    except subprocess.TimeoutExpired:
        raise RuntimeError("timed out after 120 s")

    wall_elapsed = time.perf_counter() - wall_start

    if proc.returncode != 0:
        raise RuntimeError(f"non-zero exit {proc.returncode}: {proc.stderr.strip()}")

    elapsed = None
    result = None
    for line in proc.stdout.splitlines():
        m = re.match(r"elapsed:\s*(.+)", line)
        if m:
            elapsed = float(m.group(1))
        m = re.match(r"result:\s*(.+)", line)
        if m:
            result = m.group(1).strip()

    if elapsed is None:
        # Fall back to wall time if the benchmark doesn't print elapsed:
        elapsed = wall_elapsed
    if result is None:
        result = "<no result>"

    return elapsed, result


def verify(name, result):
    expected = EXPECTED_RESULTS.get(name)
    if expected is None:
        return True  # no expectation registered
    return result.startswith(expected)


def col(s, w, right=False):
    s = str(s)
    return s.rjust(w) if right else s.ljust(w)


def main():
    args = parse_args()

    if not os.path.isfile(args.interpreter):
        print(f"error: interpreter not found: {args.interpreter}", file=sys.stderr)
        print("hint: run  cmake --preset release && cmake --build build", file=sys.stderr)
        sys.exit(1)

    benchmarks = discover_benchmarks(args.filter)
    if not benchmarks:
        print(f"No benchmarks found in {BENCHMARKS_DIR!r} (filter={args.filter!r})")
        sys.exit(0)

    print(f"Interpreter : {args.interpreter}")
    print(f"Benchmarks  : {BENCHMARKS_DIR}")
    print(f"Iterations  : {args.iterations}")
    print()

    all_results = {}
    rows = []
    any_fail = False

    for path in benchmarks:
        name = os.path.splitext(os.path.basename(path))[0]
        timings = []
        last_result = None
        error = None

        print(f"  {name} ...", end="", flush=True)
        for i in range(args.iterations):
            try:
                elapsed, result = run_once(args.interpreter, path)
                timings.append(elapsed)
                last_result = result
                print(".", end="", flush=True)
            except RuntimeError as e:
                error = str(e)
                print("E", end="", flush=True)
                break

        print()

        if error:
            all_results[name] = {"error": error}
            rows.append((name, "ERROR", "", "", error, False))
            any_fail = True
            continue

        median = statistics.median(timings)
        lo = min(timings)
        hi = max(timings)
        ok = verify(name, last_result)
        if not ok:
            any_fail = True

        all_results[name] = {
            "timings": timings,
            "median": median,
            "min": lo,
            "max": hi,
            "result": last_result,
            "ok": ok,
        }
        rows.append((name, f"{median:.3f}", f"{lo:.3f}", f"{hi:.3f}", last_result, ok))

    # Print table
    print()
    hdr = ("Benchmark", "Median (s)", "Min (s)", "Max (s)", "Result", "OK")
    widths = (14, 10, 9, 9, 14, 4)
    sep = "  ".join("-" * w for w in widths)
    header = "  ".join(col(h, w) for h, w in zip(hdr, widths))
    print(header)
    print(sep)
    for name, med, lo, hi, result, ok in rows:
        ok_str = "OK" if ok else "FAIL"
        print("  ".join(col(v, w, right=(i in (1, 2, 3)))
                        for i, (v, w) in enumerate(zip(
                            (name, med, lo, hi, result, ok_str), widths))))

    if args.json:
        with open(args.json, "w") as f:
            json.dump(all_results, f, indent=2)
        print(f"\nRaw results written to {args.json}")

    if any_fail:
        print("\nWARNING: one or more benchmarks failed correctness check.")
        sys.exit(1)


if __name__ == "__main__":
    main()
