#!/usr/bin/env python3
# mutate.py [-j N] <loxpp> <redis.lox> <run_stages.py> <mutations/GROUP.json>
#
# Proves that the stage cases can fail. For each mutation (old, new, cases),
# it writes a copy of redis.lox with that replacement (or edits), runs the stage
# suite against the copy, and checks that each named case fails. A suite
# that crashes or times out on the copy also counts as a kill. Prints one
# line per mutation, and exits 1 when a mutation stays alive or its old text
# is not found exactly once. CI does not run it: it starts one server per
# mutation and takes minutes. Set REDIS_STAGE_GROUPS (see run_stages.py) to
# run only the group of the named cases.
#
# -j N runs N mutations at once. The default is a quarter of the CPU cores:
# the stall probes have 1 s limits and measure CPU time, so a high load can
# fail a named case with no real defect, and a failing named case is a kill.
# The lines print in the order of the mutation file, whatever N is.
import argparse
import concurrent.futures
import json
import os
import subprocess
import sys
import tempfile


def run_mutation(m, text, loxpp, suite):
    """Returns (line, bad) for one mutation. bad is True when it is alive or its old text is not unique."""
    cases = m["cases"]
    # A mutation that needs more than one site gives "edits", a list of
    # [old, new] pairs applied in order; each old text must occur once.
    edits = m.get("edits") or [[m["old"], m["new"]]]
    mutant = text
    for old, new in edits:
        if mutant.count(old) != 1:
            return ("SKIP  %-40s old text found %d times: %r"
                    % (m["name"], mutant.count(old), old[:60]), True)
        mutant = mutant.replace(old, new)
    d = tempfile.mkdtemp(prefix="mut-")
    lox = os.path.join(d, "redis.lox")
    with open(lox, "w") as f:
        f.write(mutant)
    wrap = os.path.join(d, "wrap.sh")
    with open(wrap, "w") as f:
        f.write('#!/bin/sh\nexec %s %s "$@"\n' % (loxpp, lox))
    os.chmod(wrap, 0o755)
    try:
        out = subprocess.run([sys.executable, "-u", suite, wrap], capture_output=True,
                             text=True, timeout=420).stdout
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode() if isinstance(e.stdout, bytes) else (e.stdout or "")
        out += "\n(timeout)"
    failed = [c for c in cases if ("FAIL " + c) in out]
    missing = [c for c in cases if c not in failed]
    if missing and "(timeout)" not in out and "group ran to the end" not in out:
        return ("ALIVE %-40s cases that still pass: %s" % (m["name"], missing), True)
    how = "killed" if not missing else "killed (suite crash/timeout)"
    return ("%s %-40s" % (how, m["name"]), False)


def main():
    default_jobs = max(1, (os.cpu_count() or 1) // 4)
    ap = argparse.ArgumentParser(description="Run each mutation of a group against the stage suite.")
    ap.add_argument("-j", dest="jobs", type=int, default=default_jobs, metavar="N",
                    help="mutations to run at once (default: %(default)s, a quarter of the CPU cores)")
    ap.add_argument("loxpp")
    ap.add_argument("source")
    ap.add_argument("suite")
    ap.add_argument("spec")
    args = ap.parse_args()
    if args.jobs < 1:
        ap.error("-j needs N >= 1")

    text = open(args.source).read()
    mutations = json.load(open(args.spec))
    bad = 0
    pool = concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs)
    try:
        futures = [pool.submit(run_mutation, m, text, args.loxpp, args.suite) for m in mutations]
        for future in futures:
            line, is_bad = future.result()
            print(line)
            bad += is_bad
    finally:
        # An interrupt must not start the queued mutations: they would run for minutes after Ctrl-C.
        pool.shutdown(wait=True, cancel_futures=True)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
