#!/usr/bin/env python3
# mutate.py <loxpp> <redis.lox> <run_stages.py> <mutations/GROUP.json>
#
# Proves that the stage cases can fail. For each mutation (old, new, cases),
# it writes a copy of redis.lox with that one replacement, runs the stage
# suite against the copy, and checks that each named case fails. A suite
# that crashes or times out on the copy also counts as a kill. Prints one
# line per mutation, and exits 1 when a mutation stays alive or its old text
# is not found exactly once. CI does not run it: it starts one server per
# mutation and takes minutes.
import json
import os
import subprocess
import sys
import tempfile

loxpp, source, suite, spec = sys.argv[1:5]
text = open(source).read()
bad = 0
for m in json.load(open(spec)):
    old, new, cases = m["old"], m["new"], m["cases"]
    if text.count(old) != 1:
        print("SKIP  %-40s old text found %d times" % (m["name"], text.count(old)))
        bad += 1
        continue
    d = tempfile.mkdtemp(prefix="mut-")
    lox = os.path.join(d, "redis.lox")
    open(lox, "w").write(text.replace(old, new))
    wrap = os.path.join(d, "wrap.sh")
    open(wrap, "w").write('#!/bin/sh\nexec %s %s "$@"\n' % (loxpp, lox))
    os.chmod(wrap, 0o755)
    try:
        out = subprocess.run([sys.executable, suite, wrap], capture_output=True,
                             text=True, timeout=420).stdout
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode() if isinstance(e.stdout, bytes) else (e.stdout or "")
        out += "\n(timeout)"
    failed = [c for c in cases if ("FAIL " + c) in out]
    missing = [c for c in cases if c not in failed]
    if missing and "(timeout)" not in out and "group ran to the end" not in out:
        print("ALIVE %-40s cases that still pass: %s" % (m["name"], missing))
        bad += 1
    else:
        how = "killed" if not missing else "killed (suite crash/timeout)"
        print("%s %-40s" % (how, m["name"]))
sys.exit(1 if bad else 0)
