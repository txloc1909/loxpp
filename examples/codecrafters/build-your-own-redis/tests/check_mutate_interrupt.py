#!/usr/bin/env python3
"""Checks that an interrupt of tests/mutate.py stops the servers of its suites.

mutate.py runs one suite per mutation, and each suite starts one server per
case. An interrupt kills the process group of each running suite, which takes
the servers of that suite with it. SIGINT reached that cleanup, SIGTERM ended
the script at once, and SIGKILL runs no handler at all (issue #1238). A server
that stays up keeps its port, so the next run of the suite meets it as a busy
port and the case that uses it fails with no defect behind it.

Each of the three signals runs the real mutate.py, with one mutation of a stub
source and a stub suite that starts one stub server of its own, as
run_stages.py starts its servers. The suite of the SIGINT and SIGTERM cases
starts no watch of its parent, so those two cases hold the cleanup of mutate.py
itself. The suite of the SIGKILL case starts the watch, because a SIGKILL runs
no handler of mutate.py at all. A fourth case runs the real run_stages.py, so
that a change which drops the watch of main() fails here too, and not only a
change of the watch itself.

Usage:
    tests/check_mutate_interrupt.py

Exit status 0 when every case holds, 1 when a case fails.
"""

import json
import os
import signal
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MUTATE = os.path.join(HERE, "mutate.py")
RUN_STAGES = os.path.join(HERE, "run_stages.py")
# A suite needs a while to start: a mutation run of a real group starts four.
START_SECONDS = 60
# A killed server ends at once, and the watch of a suite sees its parent go
# within one interval of 0.2 s.
GONE_SECONDS = 15

# One server of its own, in the process group of the suite, as run_stages.py
# starts the servers that it drives. The marker is in the command line of the
# server only, so a case can list the servers of its mutation. A case that holds
# the cleanup of mutate.py starts no watch of the parent here.
STUB_SUITE = '''#!/usr/bin/env python3
import os
import subprocess
import sys
import time

sys.path.insert(0, os.environ["MUTATE_PROBE_TESTS"])
import run_stages

if os.environ.get("MUTATE_PROBE_WATCH"):
    run_stages.watch_parent()
server = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(3600)",
                           os.environ["MUTATE_PROBE_MARKER"]])
with open(os.environ["MUTATE_PROBE_PIDFILE"], "w") as f:
    f.write(str(server.pid))
time.sleep(3600)
'''

# A parent of a suite of its own, as mutate.py is: it gives the suite a session
# of its own and then waits, and a SIGKILL of it runs no handler of it.
STUB_PARENT = '''#!/usr/bin/env python3
import os
import subprocess
import sys
import time

suite = subprocess.Popen([sys.executable, "-u", os.environ["MUTATE_PROBE_SUITE"],
                          os.environ["MUTATE_PROBE_PROG"]],
                         start_new_session=True)
with open(os.environ["MUTATE_PROBE_PIDFILE"], "w") as f:
    f.write(str(suite.pid))
time.sleep(3600)
'''

# A program that never listens. A suite of the real run_stages.py spends its
# first seconds waiting for it, which is the window that the fourth case needs.
STUB_PROG = '#!/bin/sh\nsleep 300\n'

FAILED = 0


def check(name, cond, detail=""):
    global FAILED
    if cond:
        print("  ok   %s" % name)
    else:
        FAILED += 1
        print("  FAIL %s  %s" % (name, detail))


def live(pid):
    try:
        with open("/proc/%d/stat" % pid, "rb") as f:
            return f.read().rsplit(b")", 1)[1].split()[0] != b"Z"
    except OSError:
        return False


def pids_naming(text):
    """The live processes whose command line holds text. A process that was
    killed under an init that does not reap stays as a zombie, which runs
    nothing and must not count as a server that is left."""
    found = []
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        try:
            with open("/proc/%s/cmdline" % name, "rb") as f:
                cmdline = f.read()
        except OSError:
            continue
        if text.encode() in cmdline and live(int(name)):
            found.append(int(name))
    return found


def wait_for(pidfile, seconds):
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            with open(pidfile) as f:
                text = f.read().strip()
        except OSError:
            text = ""
        if text.isdigit():
            return int(text)
        time.sleep(0.05)
    return None


def wait_for_text(path, text, seconds):
    """Waits until a file holds text. Empty when the time is up."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            with open(path, errors="replace") as f:
                seen = f.read()
        except OSError:
            seen = ""
        if text in seen:
            return seen
        time.sleep(0.05)
    return ""


def wait_gone(names, pid, seconds):
    """Waits until nothing names a text of `names` and the pid has ended.
    Returns the processes that still run when the time is up."""
    deadline = time.time() + seconds
    while True:
        left = {p for text in names if text for p in pids_naming(text)}
        if pid is not None and live(pid):
            left.add(pid)
        if not left or time.time() > deadline:
            return sorted(left)
        time.sleep(0.1)


def kill_all(names, pid):
    """Ends what a case leaves behind, so the next case does not meet it."""
    for target in sorted({p for text in names if text for p in pids_naming(text)}):
        try:
            os.kill(target, signal.SIGKILL)
        except OSError:
            pass
    if pid is not None and live(pid):
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass


def mutation_case(sig, watch):
    """One signal, one mutation: mutate.py ends, and its suite's server ends."""
    with tempfile.TemporaryDirectory(prefix="mutprobe-") as d:
        marker = "mutate-probe-%d-%d" % (os.getpid(), int(sig))
        pidfile = os.path.join(d, "server.pid")
        source = os.path.join(d, "source.lox")
        with open(source, "w") as f:
            f.write("OLD\n")
        spec = os.path.join(d, "one.json")
        with open(spec, "w") as f:
            json.dump([{"name": "stub", "old": "OLD", "new": "NEW",
                        "cases": ["stub: the mutation is killed"]}], f)
        # mutate.py refuses a binary that is not an executable file. The stub
        # suite never runs it: it starts the server that this case watches.
        loxpp = os.path.join(d, "loxpp")
        with open(loxpp, "w") as f:
            f.write("#!/bin/sh\nexit 0\n")
        os.chmod(loxpp, 0o755)
        suite = os.path.join(d, "suite.py")
        with open(suite, "w") as f:
            f.write(STUB_SUITE)
        os.chmod(suite, 0o755)
        # The output goes to a file: a server that survives holds the write end
        # of a pipe, and a read of that pipe would wait for the server that this
        # case holds to be gone.
        out = os.path.join(d, "mutate.out")
        env = dict(os.environ, MUTATE_PROBE_TESTS=HERE, MUTATE_PROBE_MARKER=marker,
                   MUTATE_PROBE_PIDFILE=pidfile)
        if watch:
            env["MUTATE_PROBE_WATCH"] = "1"
        with open(out, "w") as log:
            proc = subprocess.Popen([sys.executable, "-u", MUTATE, loxpp, source, suite, spec],
                                    stdout=log, stderr=subprocess.STDOUT, env=env,
                                    start_new_session=True)
            try:
                server = wait_for(pidfile, START_SECONDS)
                if server is None:
                    return {"server": None, "left": [], "status": None,
                            "out": open(out).read()}
                proc.send_signal(sig)
                status = proc.wait(timeout=START_SECONDS)
                left = wait_gone([marker], server, GONE_SECONDS)
                return {"server": server, "left": left, "status": status,
                        "out": open(out).read()}
            finally:
                kill_all([marker, suite, d], None)
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()


def check_mutation_case(sig, name, watch, status=None):
    result = mutation_case(sig, watch)
    if result.get("server") is None:
        check("%s: the mutation suite starts its server" % name, False,
              "no server pid within %d s:\n%s"
              % (START_SECONDS, (result.get("out") or "")[-2000:]))
        return
    out = result["out"]
    check("%s: mutate.py ends" % name, result["status"] is not None)
    if status is not None:
        check("%s: the exit status is %d" % (name, status), result["status"] == status,
              "got %r" % (result["status"],))
    check("%s: no server is left" % name, not result["left"],
          "pids %s still run:\n%s" % (result["left"], out[-2000:]))
    check("%s: mutate.py reports no traceback" % name, "Traceback" not in out,
          out[-2000:])


def suite_case():
    """The real suite, started by a parent as mutate.py starts it. The parent is
    killed with SIGKILL, which runs no handler of the parent, so the suite ends
    itself and its "server" with it."""
    with tempfile.TemporaryDirectory(prefix="mutprobe-") as d:
        pidfile = os.path.join(d, "suite.pid")
        prog = os.path.join(d, "prog.sh")
        with open(prog, "w") as f:
            f.write(STUB_PROG)
        os.chmod(prog, 0o755)
        parent = os.path.join(d, "parent.py")
        with open(parent, "w") as f:
            f.write(STUB_PARENT)
        out = os.path.join(d, "parent.out")
        env = dict(os.environ, MUTATE_PROBE_SUITE=RUN_STAGES, MUTATE_PROBE_PROG=prog,
                   MUTATE_PROBE_PIDFILE=pidfile)
        with open(out, "w") as log:
            proc = subprocess.Popen([sys.executable, "-u", parent],
                                    stdout=log, stderr=subprocess.STDOUT, env=env)
            try:
                suite = wait_for(pidfile, START_SECONDS)
                if suite is None:
                    check("SIGKILL of a suite's parent: the suite starts", False,
                          "no suite pid within %d s:\n%s"
                          % (START_SECONDS, open(out).read()[-2000:]))
                    return
                # A suite that has printed its first group name has started its
                # watch. A kill that comes earlier leaves it a parent that it
                # never saw, and no watch of a parent can see that one.
                if not wait_for_text(out, "== ", START_SECONDS):
                    check("SIGKILL of a suite's parent: the suite starts its watch", False,
                          "no group name within %d s:\n%s"
                          % (START_SECONDS, open(out).read()[-2000:]))
                    return
                proc.kill()
                proc.wait(timeout=START_SECONDS)
                left = wait_gone([d], suite, GONE_SECONDS)
                check("SIGKILL of a suite's parent: the suite ends", not live(suite),
                      "pid %d still runs:\n%s" % (suite, open(out).read()[-2000:]))
                check("SIGKILL of a suite's parent: no server is left", not left,
                      "pids %s still run:\n%s" % (left, open(out).read()[-2000:]))
            finally:
                kill_all([d], None)
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()


def main():
    print("== mutate.py interrupts")
    # SIGINT is the interrupt that Ctrl-C gives: it holds that this file can see
    # a leak at all. No watch of the parent runs here, so the two cases that the
    # cleanup of mutate.py covers hold that cleanup alone.
    check_mutation_case(signal.SIGINT, "SIGINT", watch=False, status=128 + signal.SIGINT)
    # SIGTERM ended the script at once, so the suites kept their servers.
    check_mutation_case(signal.SIGTERM, "SIGTERM", watch=False, status=128 + signal.SIGTERM)
    # SIGKILL runs no handler of mutate.py, so only a watch of the parent in the
    # suite can end its servers.
    check_mutation_case(signal.SIGKILL, "SIGKILL", watch=True, status=-signal.SIGKILL)
    # The same watch, in the suite that mutation runs use.
    suite_case()
    print("\n%d failed" % FAILED)
    return 0 if FAILED == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
