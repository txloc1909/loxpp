#!/usr/bin/env python3
"""Check that the stop-the-world collector collects at the same points.

Usage: check_gc_stw_counts.py <loxpp-binary>
       check_gc_stw_counts.py --update <loxpp-binary>

Each large-heap program runs with smaller constants (tools/check_gc_large_heap.py
holds them) and a GC trace. For each collection, the check compares the cause,
the object counts and the byte counts before and after with the pinned file
benchmarks/gc_large_heap/stw_counts.txt. The file comes from the collector as it was
before the collector interface (issue #1280). A change to the collector, to the
heap layout or to the size of an object that is on purpose changes these
numbers, and then `--update` writes the file again. A change that is not on
purpose is the failure that this check exists to find.

Exit status: 0 pass, 1 fail, 2 usage error or a binary that cannot run, 125
skipped (a build that traces execution prints too much to run these programs
quickly).
"""

import os
import subprocess
import sys
import tempfile

from runner_precondition import refuse_unrunnable, require_runner

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_gc_large_heap as large  # noqa: E402
import gc_report  # noqa: E402

PINNED = os.path.join(large.PROGRAMS, "stw_counts.txt")
HEADER = (
    "# Pinned by tools/check_gc_stw_counts.py. One line for each collection of\n"
    "# each large-heap program with the smaller constants of\n"
    "# tools/check_gc_large_heap.py: program, number, cause, objs_before,\n"
    "# objs_after, bytes_before, bytes_after.\n"
)


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def measure(binary):
    """The lines of the pinned file, from a run of each program."""
    lines = []
    env = {k: v for k, v in os.environ.items() if k != "LOXPP_STRESS_GC"}
    env.pop("LOXPP_GC", None)
    with tempfile.TemporaryDirectory() as d:
        for name, (consts, _want) in sorted(large.CASES.items()):
            with open(os.path.join(large.PROGRAMS, f"{name}.lox")) as f:
                source = large.scaled(f.read(), consts, name)
            prog = os.path.join(d, f"{name}.lox")
            trace = os.path.join(d, f"{name}.trace")
            with open(prog, "w") as f:
                f.write(source)
            try:
                r = subprocess.run(
                    [binary, prog],
                    env={**env, "LOXPP_GC_TRACE": trace},
                    cwd=d,
                    capture_output=True,
                    text=True,
                    timeout=large.TIMEOUT_S,
                )
            except subprocess.TimeoutExpired:
                fail(f"{name}: no end in {large.TIMEOUT_S} s")
            except OSError as exc:
                refuse_unrunnable(binary, exc)
            if r.returncode != 0:
                fail(f"{name}: exit {r.returncode}: {r.stderr[:200]}")
            gcs = gc_report.parse_records(trace)["gcs"]
            for i, g in enumerate(gcs, 1):
                lines.append(
                    f"{name} {i} {g['cause']} {g['objs_before']} {g['objs_after']} "
                    f"{g['bytes_before']} {g['bytes_after']}"
                )
    return lines


def main():
    args = sys.argv[1:]
    update = False
    if args and args[0] == "--update":
        update = True
        args = args[1:]
    if len(args) != 1:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    require_runner(args[0])
    if os.environ.get("LOXPP_EXEC_TRACE_ENABLED") == "1":
        print("skipped: build traces execution")
        sys.exit(125)
    got = measure(os.path.abspath(args[0]))
    if update:
        with open(PINNED, "w") as f:
            f.write(HEADER + "\n".join(got) + "\n")
        print(f"wrote {len(got)} lines to {PINNED}")
        return
    with open(PINNED) as f:
        want = [l.rstrip("\n") for l in f if l.strip() and not l.startswith("#")]
    if got != want:
        for i, (a, b) in enumerate(zip(got, want)):
            if a != b:
                fail(f"line {i + 1} differs:\n  now    {a}\n  pinned {b}")
        fail(f"{len(got)} collections now, {len(want)} pinned")
    print("OK")


if __name__ == "__main__":
    main()
