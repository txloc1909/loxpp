#!/usr/bin/env python3
"""Multi-language, multi-suite benchmark runner for Lox++.

Runs benchmarks defined in manifest.toml via podman containers (or directly
on the host with --no-container for lox only), normalising heterogeneous output
formats into a single results table.

Usage examples
--------------
# Run all lox++ benchmarks without a container (needs build/loxpp):
  python benchmarks/runner.py --lang lox --no-container

# Run a single benchmark across all languages:
  python benchmarks/runner.py --bench fib

# Run the full AWFY suite for lox++ and js:
  python benchmarks/runner.py --suite awfy --lang lox js

# Run and emit JSON:
  python benchmarks/runner.py --suite awfy --lang lox --format json
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
import tomllib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

BENCHMARKS_DIR = Path(__file__).parent
MANIFEST = BENCHMARKS_DIR / "manifest.toml"
LANGS_CFG = BENCHMARKS_DIR / "langs.toml"
RESULTS_DIR = BENCHMARKS_DIR / "results"


# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------

@dataclass
class RunResult:
    bench: str
    lang: str
    suite: str
    adapter: str
    ok: bool
    wall_ms: float
    iter_us: list[float] = field(default_factory=list)  # per-iteration runtimes
    average_us: Optional[float] = None
    total_us: Optional[float] = None
    stdout: str = ""
    error: str = ""


# ---------------------------------------------------------------------------
# Output adapters
# ---------------------------------------------------------------------------

# Matches integers, decimals, and scientific notation (e.g. 3.99692e+06)
_NUM = r"[\d.eE+\-]+"


def _parse_awfy(stdout: str, bench: str) -> tuple[list[float], Optional[float], Optional[float]]:
    """Parse AWFY-format lines:  Name: iterations=1 runtime: X us

    Lox++ may emit large numbers in scientific notation (e.g. 3.99e+06 us),
    so we match _NUM instead of the narrower [\\d.]+ pattern.
    Falls back to computing average from per-iteration data if the summary
    line is absent or unparseable.
    """
    iter_us: list[float] = []
    average_us: Optional[float] = None
    total_us: Optional[float] = None
    for line in stdout.splitlines():
        m = re.search(rf"runtime:\s*({_NUM})\s*us", line)
        if m and "average:" not in line:
            iter_us.append(float(m.group(1)))
        m2 = re.search(rf"average:\s*({_NUM})\s*us\s+total:\s*({_NUM})\s*us", line)
        if m2:
            average_us = float(m2.group(1))
            total_us = float(m2.group(2))
    if average_us is None and iter_us:
        average_us = sum(iter_us) / len(iter_us)
        total_us = sum(iter_us)
    return iter_us, average_us, total_us


def _parse_wren(stdout: str) -> tuple[list[float], Optional[float], Optional[float]]:
    """Parse Wren benchmark output: last line 'elapsed: X' (seconds)."""
    iter_us = []
    for line in stdout.splitlines():
        m = re.search(r"elapsed:\s*([\d.]+)", line)
        if m:
            iter_us.append(float(m.group(1)) * 1_000_000)
    avg = sum(iter_us) / len(iter_us) if iter_us else None
    total = sum(iter_us) if iter_us else None
    return iter_us, avg, total


def _parse_clbg(wall_ms: float) -> tuple[list[float], Optional[float], Optional[float]]:
    """CLBG: no self-timing — use wall clock (single measurement)."""
    wall_us = wall_ms * 1000
    return [wall_us], wall_us, wall_us


def _parse_clox(stdout: str) -> tuple[list[float], Optional[float], Optional[float]]:
    """clox benchmarks print: result-line, then elapsed-seconds on the last line."""
    lines = [ln.strip() for ln in stdout.splitlines() if ln.strip()]
    if len(lines) >= 2:
        try:
            elapsed_us = float(lines[-1]) * 1_000_000
            return [elapsed_us], elapsed_us, elapsed_us
        except ValueError:
            pass
    return [], None, None


def _parse_raw(wall_ms: float) -> tuple[list[float], Optional[float], Optional[float]]:
    """Fallback: use wall-clock time."""
    wall_us = wall_ms * 1000
    return [wall_us], wall_us, wall_us


# ---------------------------------------------------------------------------
# Subprocess execution
# ---------------------------------------------------------------------------

def run_in_container(
    image: str,
    cmd: list[str],
    mount: Optional[str],
    timeout: int,
) -> tuple[str, str, float, int]:
    podman_cmd = ["podman", "run", "--rm"]
    if mount:
        host_src, rest = mount.split(":", 1)
        abs_src = str(BENCHMARKS_DIR.parent / host_src)
        podman_cmd += ["-v", f"{abs_src}:{rest}"]
    podman_cmd += [image] + cmd
    t0 = time.monotonic()
    proc = subprocess.run(
        podman_cmd,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    wall_ms = (time.monotonic() - t0) * 1000
    return proc.stdout, proc.stderr, wall_ms, proc.returncode


def run_direct(
    cmd: list[str],
    timeout: int,
) -> tuple[str, str, float, int]:
    t0 = time.monotonic()
    proc = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    wall_ms = (time.monotonic() - t0) * 1000
    return proc.stdout, proc.stderr, wall_ms, proc.returncode


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------

def _resolve_cmd(lang_cfg: dict, file: str) -> list[str]:
    return [part.replace("{file}", file) for part in lang_cfg["cmd"]]


def run_benchmark(
    bench: str,
    lang: str,
    bench_cfg: dict,
    lang_cfg: dict,
    suite: str,
    no_container: bool,
    lox_binary: Optional[str],
    timeout: int,
) -> RunResult:
    adapter = bench_cfg.get("adapter", lang_cfg.get("adapter", "awfy"))
    file = bench_cfg["file"]
    cmd = _resolve_cmd(lang_cfg, file)

    # --no-container only affects lox++; all other languages always use containers.
    if lang == "lox" and no_container:
        binary = lox_binary or str(BENCHMARKS_DIR.parent / "build" / "loxpp")
        lox_file = str(BENCHMARKS_DIR / "lox" / file)
        cmd = [binary, lox_file]

    try:
        if lang == "lox" and no_container:
            stdout, stderr, wall_ms, rc = run_direct(cmd, timeout)
        else:
            mount = lang_cfg.get("mount")
            stdout, stderr, wall_ms, rc = run_in_container(
                lang_cfg["image"], cmd, mount, timeout
            )
    except subprocess.TimeoutExpired:
        return RunResult(bench, lang, suite, adapter, ok=False,
                         wall_ms=0, error="timeout")
    except FileNotFoundError as e:
        return RunResult(bench, lang, suite, adapter, ok=False,
                         wall_ms=0, error=str(e))

    ok = rc == 0
    if adapter == "awfy":
        iter_us, avg, total = _parse_awfy(stdout, bench)
    elif adapter == "wren":
        iter_us, avg, total = _parse_wren(stdout)
    elif adapter == "clbg":
        iter_us, avg, total = _parse_clbg(wall_ms)
    elif adapter == "clox":
        iter_us, avg, total = _parse_clox(stdout)
    else:  # raw
        iter_us, avg, total = _parse_raw(wall_ms)

    return RunResult(
        bench=bench, lang=lang, suite=suite, adapter=adapter, ok=ok,
        wall_ms=wall_ms, iter_us=iter_us, average_us=avg, total_us=total,
        stdout=stdout, error=stderr if not ok else "",
    )


# ---------------------------------------------------------------------------
# Output formatters
# ---------------------------------------------------------------------------

def _fmt_us(us: Optional[float]) -> str:
    if us is None:
        return "N/A"
    if us >= 1_000_000:
        return f"{us / 1_000_000:.2f}s"
    if us >= 1_000:
        return f"{us / 1_000:.1f}ms"
    return f"{us:.0f}µs"


def format_table(results: list[RunResult]) -> str:
    langs = sorted({r.lang for r in results})
    benches = sorted({r.bench for r in results})
    by_key = {(r.bench, r.lang): r for r in results}

    col_w = max(12, max((len(b) for b in benches), default=0))
    lang_w = 10

    header = f"{'Benchmark':<{col_w}}" + "".join(f"  {l:>{lang_w}}" for l in langs)
    sep = "-" * len(header)
    lines = [header, sep]

    for bench in benches:
        row = f"{bench:<{col_w}}"
        for lang in langs:
            r = by_key.get((bench, lang))
            if r is None:
                cell = "-"
            elif not r.ok:
                cell = "ERR"
            else:
                cell = _fmt_us(r.average_us)
            row += f"  {cell:>{lang_w}}"
        lines.append(row)

    return "\n".join(lines)


def format_markdown(results: list[RunResult]) -> str:
    langs = sorted({r.lang for r in results})
    benches = sorted({r.bench for r in results})
    by_key = {(r.bench, r.lang): r for r in results}

    header = "| Benchmark | " + " | ".join(langs) + " |"
    sep = "|---|" + "|".join(["---"] * len(langs)) + "|"
    lines = [header, sep]

    for bench in benches:
        row = f"| {bench} |"
        for lang in langs:
            r = by_key.get((bench, lang))
            if r is None:
                cell = "-"
            elif not r.ok:
                cell = "ERR"
            else:
                cell = _fmt_us(r.average_us)
            row += f" {cell} |"
        lines.append(row)

    return "\n".join(lines)


def format_json(results: list[RunResult]) -> str:
    data = []
    for r in results:
        data.append({
            "bench": r.bench,
            "lang": r.lang,
            "suite": r.suite,
            "ok": r.ok,
            "wall_ms": r.wall_ms,
            "iter_us": r.iter_us,
            "average_us": r.average_us,
            "total_us": r.total_us,
            "error": r.error,
        })
    return json.dumps(data, indent=2)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(description="Lox++ multi-language benchmark runner")
    parser.add_argument("--bench", nargs="+", help="Benchmark name(s) to run (default: all)")
    parser.add_argument("--lang", nargs="+", help="Language(s) to run (default: all)")
    parser.add_argument("--suite", choices=["awfy", "clbg", "wren"], help="Filter by suite")
    parser.add_argument("--no-container", action="store_true",
                        help="Run lox++ directly without podman (uses --lox-binary or build/loxpp)")
    parser.add_argument("--lox-binary", help="Path to loxpp binary (for --no-container)")
    parser.add_argument("--format", choices=["table", "markdown", "json"], default="table")
    parser.add_argument("--timeout", type=int, default=120, help="Per-run timeout (seconds)")
    parser.add_argument("--save", action="store_true", help="Save results to benchmarks/results/")
    args = parser.parse_args()

    with open(MANIFEST, "rb") as f:
        manifest = tomllib.load(f)
    with open(LANGS_CFG, "rb") as f:
        langs_cfg = tomllib.load(f)

    # Collect (bench, lang) pairs to run
    runs: list[tuple[str, str, dict, dict, str]] = []
    for bench_name, bench_data in manifest.items():
        if not isinstance(bench_data, dict):
            continue
        suite = bench_data.get("suite", "")
        if args.suite and suite != args.suite:
            continue
        if args.bench and bench_name not in args.bench:
            continue
        for lang, lang_bench in bench_data.items():
            if lang in ("suite",):
                continue
            if not isinstance(lang_bench, dict):
                continue
            if args.lang and lang not in args.lang:
                continue
            if lang not in langs_cfg:
                continue
            runs.append((bench_name, lang, lang_bench, langs_cfg[lang], suite))

    if not runs:
        print("No matching benchmarks found.", file=sys.stderr)
        sys.exit(1)

    results = []
    for bench_name, lang, bench_cfg, lang_cfg, suite in runs:
        print(f"  {bench_name:20s} [{lang}] ...", end=" ", flush=True)
        r = run_benchmark(
            bench=bench_name,
            lang=lang,
            bench_cfg=bench_cfg,
            lang_cfg=lang_cfg,
            suite=suite,
            no_container=args.no_container,
            lox_binary=args.lox_binary,
            timeout=args.timeout,
        )
        status = _fmt_us(r.average_us) if r.ok else f"ERROR: {r.error[:60]}"
        print(status)
        results.append(r)

    print()
    if args.format == "table":
        print(format_table(results))
    elif args.format == "markdown":
        print(format_markdown(results))
    else:
        print(format_json(results))

    if args.save:
        RESULTS_DIR.mkdir(exist_ok=True)
        ts = time.strftime("%Y%m%d_%H%M%S")
        out = RESULTS_DIR / f"results_{ts}.json"
        out.write_text(format_json(results))
        print(f"\nSaved to {out}")


if __name__ == "__main__":
    main()
