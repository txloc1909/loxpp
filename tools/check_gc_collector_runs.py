#!/usr/bin/env python3
"""Show which collector a runner uses, and fail if it is not the expected one.

Usage: check_gc_collector_runs.py <runner> <stw|incremental>
       check_gc_collector_runs.py --self-test <runner>

A CI step picks the collector with LOXPP_GC in its environment. A step that
silently runs `stw` twice proves nothing about `incremental`, so each such step
runs this check first. It requires that the environment holds the expected
collector (an unset variable is `stw`), runs a program that makes garbage
with a trace on, and requires the records of that collector: `gc` records for
`stw`, and `cycle` and `step` records and no `gc` record for `incremental`.
--self-test runs the check with each collector, and with a collector that the
environment does not select, which must fail. The runner can be the native binary or a wrapper such as tools/loxpp_qbe.sh.
It prints one line that names the collector.

Exit status: 0 pass, 1 fail, 2 usage error or a runner that cannot run, 125
skipped (--self-test on a build that traces execution).
"""

import os
import subprocess
import sys
import tempfile

from runner_precondition import refuse_unrunnable, require_runner

PROGRAM = """\
class Node { init(i) { this.i = i; } }
var keep = [];
var i = 0;
while (i < 60000) { keep.append(Node(i)); i = i + 1; }
i = 0;
while (i < 300000) { var t = Node(i); i = i + 1; }
print len(keep);
"""


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def self_test(runner):
    if os.environ.get("LOXPP_EXEC_TRACE_ENABLED") == "1":
        print("skipped: build traces execution")
        sys.exit(125)
    me = os.path.abspath(__file__)
    for have, want, rc in (("stw", "stw", 0), ("incremental", "incremental", 0),
                           ("stw", "incremental", 1), ("incremental", "stw", 1)):
        env = {k: v for k, v in os.environ.items() if k != "LOXPP_GC"}
        env["LOXPP_GC"] = have
        r = subprocess.run([sys.executable, me, runner, want], env=env,
                           capture_output=True, text=True)
        if r.returncode != rc:
            fail(f"LOXPP_GC={have}, expected {want}: exit {r.returncode}, "
                 f"wanted {rc}: {r.stdout[:200]}{r.stderr[:200]}")
    print("OK")


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--self-test":
        require_runner(sys.argv[2])
        self_test(os.path.abspath(sys.argv[2]))
        return
    if len(sys.argv) != 3 or sys.argv[2] not in ("stw", "incremental"):
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    runner, want = os.path.abspath(sys.argv[1]), sys.argv[2]
    require_runner(runner)
    have = os.environ.get("LOXPP_GC") or "stw"
    if have != want:
        fail(f"LOXPP_GC selects {have!r}, the step expects {want!r}")
    with tempfile.TemporaryDirectory() as d:
        prog = os.path.join(d, "churn.lox")
        trace = os.path.join(d, "churn.trace")
        with open(prog, "w") as f:
            f.write(PROGRAM)
        env = {k: v for k, v in os.environ.items() if k != "LOXPP_STRESS_GC"}
        env["LOXPP_GC_TRACE"] = trace
        try:
            r = subprocess.run([runner, prog], env=env, cwd=d,
                               capture_output=True, text=True, timeout=300)
        except OSError as exc:
            refuse_unrunnable(runner, exc)
        if r.returncode != 0 or r.stdout.strip() != "60000":
            fail(f"the probe program gave exit {r.returncode}, stdout "
                 f"{r.stdout[:100]!r}, stderr {r.stderr[:200]!r}")
        names = {}
        with open(trace) as f:
            for line in f:
                if line.strip() and not line.startswith("#"):
                    k = line.split()[0]
                    names[k] = names.get(k, 0) + 1
    if want == "stw":
        if not names.get("gc") or names.get("cycle") or names.get("step"):
            fail(f"records {names}: not the trace of stw")
    elif not names.get("cycle") or not names.get("step") or names.get("gc"):
        fail(f"records {names}: not the trace of incremental")
    print(f"collector: {want} (LOXPP_GC={os.environ.get('LOXPP_GC', '<unset>')}, "
          f"{names.get('gc', 0) + names.get('cycle', 0)} collections in the probe)")


if __name__ == "__main__":
    main()
