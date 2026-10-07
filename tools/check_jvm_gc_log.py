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


def check_star_log():
    # -Xlog:gc* output, as gc_compare.py produces it: pause start lines, detail
    # lines, and a Heap block at exit. Seven pauses end in sizes. Sizes are
    # whole MB: 1M->1M, then six 2M->1M. Allocation is 1 + 6 = 7 MB over the
    # 615830548 ns of uptime at the last line.
    s = summary("jvm_gc_star_serial.log")
    check(s["collections"] == 7, f"star collections {s['collections']}")
    check(s["causes"] == {"young": 7}, f"star causes {s['causes']}")
    want = 7 * 1048576 / 0.615830548 / 1e6
    check(close(s["alloc_mb_per_s"], want), f"star alloc {s['alloc_mb_per_s']} vs {want}")
    check(close(s["gc_ms"], 1.635 + 0.581 + 0.189 + 0.220 + 0.176 + 0.168 + 0.037, 1e-9),
          f"star gc ms {s['gc_ms']}")


K_LOG = """\
[1000000ns] Using Serial
[5000000ns] GC(0) Pause Young (Allocation Failure) 1536K->512K(5M) 2.000ms
[9000000ns] GC(1) Pause Young (Allocation Failure) 2048K->1024K(5M) 1.000ms
[20000000ns] Heap
"""


def check_kilobyte_sizes():
    # Allocation is 1536K + (2048K - 512K) = 3072K = 3145728 bytes in 20 ms.
    _, _, end, gcs, _ = jvm_gc_log.parse_log(K_LOG)
    check([g["bytes_before"] for g in gcs] == [1536 * 1024, 2048 * 1024], f"K before {gcs}")
    check([g["bytes_after"] for g in gcs] == [512 * 1024, 1024 * 1024], f"K after {gcs}")
    with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as t:
        t.write(K_LOG)
    try:
        s = jvm_gc_log.summarise(t.name, [10])
    finally:
        os.unlink(t.name)
    want = 3145728 / 0.020 / 1e6
    check(close(s["alloc_mb_per_s"], want), f"K alloc {s['alloc_mb_per_s']} vs {want}")


def check_unsupported_collectors():
    zgc = (
        "[2000000ns] Using legacy single-generation mode\n"
        "[3000000ns] Using The Z Garbage Collector\n"
        "[9000000ns] GC(0) Pause Mark Start 0.010ms\n"
    )
    for name, text in (("zgc", zgc), ("no Using line", "[1000000ns] Heap\n")):
        try:
            jvm_gc_log.parse_log(text)
        except ValueError:
            continue
        check(False, f"parse_log accepted a {name} log")


def check_no_pause_log():
    with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as t:
        t.write("[1000000ns] Using G1\n[5000000ns] Heap\n")
    try:
        s = jvm_gc_log.summarise(t.name, [1])
    finally:
        os.unlink(t.name)
    check(s["collections"] == 0, "no-pause log has collections")
    check(
        s["phase_pct"] is None
        and s["mark_mobj_per_s"] is None
        and s["sweep_mobj_per_s"] is None,
        "no-pause log must print n/a for phases and rates",
    )


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
check_star_log()
check_kilobyte_sizes()
check_unsupported_collectors()
check_no_pause_log()
check_rejects_bad_input()
check_shared_math()
if failures:
    sys.exit(1)
print("OK")
