#!/usr/bin/env python3
"""Check tools/jvm_gc_log.py against checked-in JVM GC logs.

Usage: check_jvm_gc_log.py

The logs in tools/testdata/ are real -Xlog:gc output (G1 with concurrent
cycles, Serial with full collections). The expected numbers were computed from
the log lines by hand. The last check feeds one parsed pause list through the
native trace reader and requires the same numbers, so the two reports cannot
use different math.
"""

import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gc_report  # noqa: E402
import jvm_gc_log  # noqa: E402

DATA = os.path.join(HERE, "testdata")
failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)
        print(f"FAIL: {msg}")


def close(a, b, tol=1e-6):
    return abs(a - b) <= tol * max(1.0, abs(b))


def summary(name):
    return jvm_gc_log.summarise(os.path.join(DATA, name), [10, 100])


def check_g1():
    s = summary("jvm_gc_g1.log")
    check(s["collections"] == 45, f"g1 collections {s['collections']}")
    check(
        s["causes"] == {"young": 33, "remark": 6, "cleanup": 6},
        f"g1 causes {s['causes']}",
    )
    check(s["concurrent_cycles"] == 6, f"g1 concurrent cycles {s['concurrent_cycles']}")
    check(close(s["concurrent_ms"], 103.54, 1e-4), f"g1 concurrent ms {s['concurrent_ms']}")
    check(close(s["gc_ms"], 62.10, 1e-3), f"g1 gc ms {s['gc_ms']}")
    check(close(s["pause_max_us"], 4976.0), f"g1 max pause {s['pause_max_us']}")
    check(close(s["wall_ms"], 274.0, 1e-3), f"g1 wall {s['wall_ms']}")
    check(s["phase_pct"] is None, "g1 phase share must be n/a")
    check(s["mark_mobj_per_s"] is None, "g1 mark rate must be n/a")
    check(s["sweep_mobj_per_s"] is None, "g1 sweep rate must be n/a")
    text = gc_report.render(s)
    check("mark rate         n/a Mobj/s" in text, "render must print n/a for mark rate")


def check_serial():
    s = summary("jvm_gc_serial.log")
    check(s["causes"] == {"young": 67, "full": 3}, f"serial causes {s['causes']}")
    check(s["concurrent_cycles"] == 0, "serial has no concurrent cycles")
    check(close(s["pause_max_us"], 18276.0), f"serial max pause {s['pause_max_us']}")
    check(close(s["gc_ms"], 88.21, 1e-3), f"serial gc ms {s['gc_ms']}")


def check_rejects_bad_input():
    for bad in ("GC(0) Pause Young (Allocation Failure) 5M->1M(19M) 2.3ms\n", ""):
        try:
            jvm_gc_log.parse_log(bad)
        except ValueError:
            continue
        check(False, f"parse_log accepted {bad!r}")


def check_shared_math():
    path = os.path.join(DATA, "jvm_gc_g1.log")
    with open(path) as f:
        _, start, end, gcs, _ = jvm_gc_log.parse_log(f.read())
    with tempfile.NamedTemporaryFile("w", suffix=".trace", delete=False) as t:
        t.write("# loxpp-gc-trace v1\n")
        t.write(f"start t={start}\n")
        for g in gcs:
            # The native format needs phase timestamps and counts; they do
            # not enter the figures compared below.
            t.write(
                f"gc cause={g['cause']} t0={g['t0']} t1={g['t0']} t2={g['t0']}"
                f" t3={g['t0']} t4={g['t4']} bytes_before={g['bytes_before']}"
                f" bytes_after={g['bytes_after']} objs_before=0 objs_after=0"
                " marked=0 freed=0\n"
            )
        t.write(f"end t={end}\n")
    try:
        native = gc_report.summarise(t.name, [10, 100])
    finally:
        os.unlink(t.name)
    jvm = summary("jvm_gc_g1.log")
    for key in (
        "gc_overhead_pct",
        "pause_p50_us",
        "pause_p99_us",
        "pause_max_us",
        "alloc_mb_per_s",
        "mmu",
    ):
        check(native[key] == jvm[key], f"{key}: trace {native[key]} vs jvm {jvm[key]}")


check_g1()
check_serial()
check_rejects_bad_input()
check_shared_math()
if failures:
    sys.exit(1)
print("OK")
