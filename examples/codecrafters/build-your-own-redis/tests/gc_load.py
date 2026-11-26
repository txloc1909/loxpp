#!/usr/bin/env python3
# gc_load.py [your_program.sh]
#
# The load of #1165 against the incremental collector: 524286 streams, then
# `XREAD BLOCK 0` on all of them from two clients, then one XADD that wakes
# both. The server writes a GC trace. The trace goes through the pause gate of
# tools/check_gc_pause.py: the largest step in work units (a count, the main
# check), then the 99.99th percentile of the steps and MMU(10 ms).
#
# Run it with LOXPP_GC=incremental in the environment. It refuses another
# value, so a job that runs the wrong collector fails and does not pass.
#
# Exit status: 0 pass, 1 fail.
import os
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "..", "tools"))
import run_stages  # noqa: E402
import check_gc_pause as pause  # noqa: E402

# A run takes about a minute. A time failure can come from a stall of the host,
# so the load runs again, at most this many times in all. A count failure
# does not run again.
ATTEMPTS = 3
STREAMS = 524286
BATCH = 2000


def load(prog):
    """One run of the load. Returns the figures of its trace, or None."""
    trace = tempfile.NamedTemporaryFile(prefix="loxpp-redis-gc-", suffix=".trace", delete=False)
    trace.close()
    srv = run_stages.Server(prog, [], env={"LOXPP_GC_TRACE": trace.name})
    ok = False
    try:
        if not srv.up:
            print("FAIL: the server did not start\n" + srv.log_text())
            return None
        enc = run_stages.encode
        keys = ["e%d" % i for i in range(STREAMS)]
        loader = srv.client()
        began = time.time()
        for i in range(0, STREAMS, BATCH):
            chunk = keys[i:i + BATCH]
            loader.send_raw(b"".join(enc("XADD", k, "1-1", "f", "v") for k in chunk))
            for _ in chunk:
                got = loader.read()
                if got != "1-1":
                    print("FAIL: XADD gave %r" % (got,))
                    return None
        print("created %d streams in %.1f s" % (STREAMS, time.time() - began))
        waiters = [srv.client(), srv.client()]
        for w in waiters:
            w.send_raw(enc("XREAD", "BLOCK", "0", "STREAMS", *keys, *(["$"] * STREAMS)))
            # The first read of a client does not return until the XADD below.
            time.sleep(1)
        waker = srv.client()
        if waker.call("XADD", keys[0], "2-1", "f", "w") != "2-1":
            print("FAIL: the waking XADD gave a wrong ID")
            return None
        for n, w in enumerate(waiters, 1):
            w.sock.settimeout(60)
            got = w.read()
            if got != [[keys[0], [["2-1", ["f", "w"]]]]]:
                print("FAIL: waiter %d got %r" % (n, str(got)[:100]))
                return None
        print("both waiters got the entry")
        if waker.call("PING") != "PONG":
            print("FAIL: PING after the load")
            return None
        ok = True
    finally:
        srv.stop()
    try:
        if not ok:
            return None
        # The server ends on SIGTERM, which can cut the last record of the trace.
        with open(trace.name, "rb") as f:
            data = f.read()
        if not data.endswith(b"\n"):
            with open(trace.name, "wb") as f:
                f.write(data[:data.rfind(b"\n") + 1])
        return pause.measure(trace.name)
    finally:
        os.unlink(trace.name)


def main():
    prog = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else run_stages.DEFAULT_PROG)
    if os.environ.get("LOXPP_GC") != "incremental":
        print("FAIL: LOXPP_GC must be 'incremental', it is %r" % os.environ.get("LOXPP_GC"))
        return 1
    label = "redis-524286-streams"
    runs = []
    failures = ["no run"]
    for attempt in range(ATTEMPTS):
        m = load(prog)
        if m is None:
            return 1
        runs.append(m)
        failures = pause.gate(label, runs, show=False)
        print(pause.describe("%s run %d" % (label, attempt + 1), m))
        if not failures or any(x["problems"] for x in runs):
            break
    for f in failures:
        print("FAIL: " + f)
    if failures:
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
