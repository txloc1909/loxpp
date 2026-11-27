#!/usr/bin/env python3
"""Checks that an interrupt of tests/mutate.py stops the servers of its suites.

mutate.py runs one suite per mutation, and each suite starts one server per
case. An interrupt kills the process group of each running suite, which takes
the servers of that suite with it. SIGINT reached that cleanup, SIGTERM ended
the script at once, and SIGKILL runs no handler at all (#1238). A server that
stays up keeps its port, so the next run of the suite meets it as a busy port
and the case that uses it fails with no defect behind it.

Each of the three signals runs the real mutate.py, with one mutation of a stub
source and a stub suite that starts one stub server of its own, as
run_stages.py starts its servers. The suite of the SIGINT and SIGTERM cases
starts no watch of its parent, so those two cases hold the cleanup of mutate.py
itself. The suite of the SIGKILL case starts the watch, because a SIGKILL runs
no handler of mutate.py at all.

Two further cases hold the start of a suite:

- a SIGKILL of mutate.py before the suite reaches its watch. The suite then
  reads the pid of the process that reaped it, so only the pid that mutate.py
  names in LOXPP_SUITE_PARENT can end it;
- the real run_stages.py, started by a parent as mutate.py starts it, so a
  change which drops the watch of main() fails here too, and not only a change
  of the watch itself.

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
# A cleanup that still waits at this limit did not come back: the suites are
# alive, or one of them ignores the kill. The case then reports, and does not
# end with a traceback from this file.
WAIT_SECONDS = 20
# A killed server ends at once, and the watch of a suite sees its parent go
# within one interval of 0.2 s.
GONE_SECONDS = 15
# The window of the case that kills a parent before its suite starts the watch.
RACE_SECONDS = 1
# How long a cleanup that waits is given to reach its wait, before the case that
# ends it with a second signal sends that signal.
SECOND_SECONDS = 0.5

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

# A suite whose server runs before its watch starts, which is the window that a
# program spends starting. The parent is killed inside that window.
STUB_EARLY = '''#!/usr/bin/env python3
import os
import subprocess
import sys
import time

sys.path.insert(0, os.environ["MUTATE_PROBE_TESTS"])
import run_stages

server = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(3600)",
                           os.environ["MUTATE_PROBE_MARKER"]])
with open(os.environ["MUTATE_PROBE_PIDFILE"], "w") as f:
    f.write(str(server.pid))
with open(os.environ["MUTATE_PROBE_SUITE_PIDFILE"], "w") as f:
    f.write(str(os.getpid()))
with open(os.environ["MUTATE_PROBE_STARTED"], "w") as f:
    f.write("1")
time.sleep(float(os.environ["MUTATE_PROBE_RACE"]))
run_stages.watch_parent()
time.sleep(3600)
'''

# A suite whose server leads a session of its own, so the group kill of an
# interrupt does not reach it. The cleanup of mutate.py then waits for the pipe
# that the server holds, and a second signal can arrive while it waits.
STUB_SESSION_SUITE = '''#!/usr/bin/env python3
import os
import subprocess
import sys
import time

server = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(3600)",
                           os.environ["MUTATE_PROBE_MARKER"]],
                          start_new_session=True)
with open(os.environ["MUTATE_PROBE_PIDFILE"], "w") as f:
    f.write(str(server.pid))
time.sleep(3600)
'''

# A parent of a suite of its own, as mutate.py is: it gives the suite a session
# of its own, names itself as the parent, and then waits. A SIGKILL of it runs
# no handler of it.
STUB_PARENT = '''#!/usr/bin/env python3
import os
import subprocess
import sys
import time

env = dict(os.environ, LOXPP_SUITE_PARENT=str(os.getpid()))
suite = subprocess.Popen([sys.executable, "-u", os.environ["MUTATE_PROBE_SUITE"],
                          os.environ["MUTATE_PROBE_PROG"]],
                         start_new_session=True, env=env)
with open(os.environ["MUTATE_PROBE_PIDFILE"], "w") as f:
    f.write(str(suite.pid))
time.sleep(3600)
'''

# A program that never listens. A suite of the real run_stages.py spends its
# first seconds waiting for it, which is the window that the last case needs.
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


def write_mutation(d):
    """Writes a stub source, its one mutation, and a stub binary.

    mutate.py refuses a binary that is not an executable file. The stub suites
    never run it: they start the server that a case watches.
    """
    source = os.path.join(d, "source.lox")
    with open(source, "w") as f:
        f.write("OLD\n")
    spec = os.path.join(d, "one.json")
    with open(spec, "w") as f:
        json.dump([{"name": "stub", "old": "OLD", "new": "NEW",
                    "cases": ["stub: the mutation is killed"]}], f)
    loxpp = os.path.join(d, "loxpp")
    with open(loxpp, "w") as f:
        f.write("#!/bin/sh\nexit 0\n")
    os.chmod(loxpp, 0o755)
    return loxpp, source, spec


def write_script(d, name, text):
    path = os.path.join(d, name)
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, 0o755)
    return path


def start_mutate(d, suite, extra, log):
    loxpp, source, spec = write_mutation(d)
    env = dict(os.environ, MUTATE_PROBE_TESTS=HERE, **extra)
    return subprocess.Popen([sys.executable, "-u", MUTATE, loxpp, source, suite, spec],
                            stdout=log, stderr=subprocess.STDOUT, env=env,
                            start_new_session=True)


def end_run(proc, names):
    kill_all(names, None)
    if proc.poll() is None:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait()


def wait_status(proc):
    """The exit status of a script that a signal should have ended."""
    try:
        return proc.wait(timeout=WAIT_SECONDS)
    except subprocess.TimeoutExpired:
        return None


def mutation_case(sig, watch):
    """One signal, one mutation: mutate.py ends, and its suite's server ends."""
    with tempfile.TemporaryDirectory(prefix="mutprobe-") as d:
        marker = "mutate-probe-%d-%d" % (os.getpid(), int(sig))
        pidfile = os.path.join(d, "server.pid")
        suite = write_script(d, "suite.py", STUB_SUITE)
        extra = {"MUTATE_PROBE_MARKER": marker, "MUTATE_PROBE_PIDFILE": pidfile}
        if watch:
            extra["MUTATE_PROBE_WATCH"] = "1"
        # The output goes to a file: a server that survives holds the write end
        # of a pipe, and a read of that pipe would wait for the server that this
        # case holds to be gone.
        out = os.path.join(d, "mutate.out")
        with open(out, "w") as log:
            proc = start_mutate(d, suite, extra, log)
            try:
                server = wait_for(pidfile, START_SECONDS)
                if server is None:
                    return {"server": None, "left": [], "status": None,
                            "out": open(out).read()}
                proc.send_signal(sig)
                status = wait_status(proc)
                left = wait_gone([marker], server, GONE_SECONDS)
                return {"server": server, "left": left, "status": status,
                        "out": open(out).read()}
            finally:
                end_run(proc, [marker, suite, d])


def check_mutation_case(sig, name, watch, status=None):
    result = mutation_case(sig, watch)
    if result["server"] is None:
        check("%s: the mutation suite starts its server" % name, False,
              "no server pid within %d s:\n%s" % (START_SECONDS, result["out"][-2000:]))
        return
    out = result["out"]
    check("%s: mutate.py ends" % name, result["status"] is not None,
          "still runs %d s after the signal:\n%s" % (WAIT_SECONDS, out[-2000:]))
    if status is not None:
        check("%s: the exit status is %d" % (name, status), result["status"] == status,
              "got %r" % (result["status"],))
    check("%s: no server is left" % name, not result["left"],
          "pids %s still run:\n%s" % (result["left"], out[-2000:]))
    check("%s: mutate.py reports no traceback" % name, "Traceback" not in out,
          out[-2000:])


def early_case():
    """mutate.py is killed in the window in which its suite starts, before the
    suite reaches its watch. The suite then reads the pid of the process that
    reaped it, so only the pid that mutate.py names can end it."""
    with tempfile.TemporaryDirectory(prefix="mutprobe-") as d:
        marker = "mutate-probe-early-%d" % os.getpid()
        started = os.path.join(d, "started")
        suite_pidfile = os.path.join(d, "suite.pid")
        suite = write_script(d, "suite.py", STUB_EARLY)
        extra = {"MUTATE_PROBE_MARKER": marker,
                 "MUTATE_PROBE_PIDFILE": os.path.join(d, "server.pid"),
                 "MUTATE_PROBE_SUITE_PIDFILE": suite_pidfile,
                 "MUTATE_PROBE_STARTED": started,
                 "MUTATE_PROBE_RACE": str(RACE_SECONDS)}
        out = os.path.join(d, "mutate.out")
        with open(out, "w") as log:
            proc = start_mutate(d, suite, extra, log)
            try:
                wait_for(started, START_SECONDS)
                suite_pid = wait_for(suite_pidfile, START_SECONDS)
                if suite_pid is None:
                    check("SIGKILL in the start of a suite: the suite starts", False,
                          "no suite pid within %d s:\n%s"
                          % (START_SECONDS, open(out).read()[-2000:]))
                    return
                proc.kill()
                wait_status(proc)
                left = wait_gone([marker, d], suite_pid, GONE_SECONDS)
                check("SIGKILL in the start of a suite: the suite ends", not live(suite_pid),
                      "pid %d still runs:\n%s" % (suite_pid, open(out).read()[-2000:]))
                check("SIGKILL in the start of a suite: no server is left", not left,
                      "pids %s still run:\n%s" % (left, open(out).read()[-2000:]))
            finally:
                end_run(proc, [marker, suite, d])


def suite_case():
    """The real suite, started by a parent as mutate.py starts it, and killed as
    soon as the suite exists: the suite must still end, and end its server."""
    with tempfile.TemporaryDirectory(prefix="mutprobe-") as d:
        pidfile = os.path.join(d, "suite.pid")
        prog = write_script(d, "prog.sh", STUB_PROG)
        parent = write_script(d, "parent.py", STUB_PARENT)
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
                proc.kill()
                wait_status(proc)
                left = wait_gone([d], suite, GONE_SECONDS)
                check("SIGKILL of a suite's parent: the suite ends", not live(suite),
                      "pid %d still runs:\n%s" % (suite, open(out).read()[-2000:]))
                check("SIGKILL of a suite's parent: no server is left", not left,
                      "pids %s still run:\n%s" % (left, open(out).read()[-2000:]))
            finally:
                end_run(proc, [d])


def second_signal_case(first, second):
    """A cleanup that waits must end at once on a second signal, of either kind.

    The first signal runs the handler, which gives both signals their default
    action; a second signal must then end the process, and not reach a handler
    that does nothing because a cleanup is already running.
    """
    with tempfile.TemporaryDirectory(prefix="mutprobe-") as d:
        name = "second %s" % signal.Signals(second).name
        marker = "mutate-probe-second-%d-%d" % (os.getpid(), int(second))
        pidfile = os.path.join(d, "server.pid")
        suite = write_script(d, "suite.py", STUB_SESSION_SUITE)
        extra = {"MUTATE_PROBE_MARKER": marker, "MUTATE_PROBE_PIDFILE": pidfile}
        out = os.path.join(d, "mutate.out")
        with open(out, "w") as log:
            proc = start_mutate(d, suite, extra, log)
            try:
                server = wait_for(pidfile, START_SECONDS)
                if server is None:
                    check("%s: the mutation suite starts its server" % name, False,
                          "no server pid within %d s:\n%s"
                          % (START_SECONDS, open(out).read()[-2000:]))
                    return
                proc.send_signal(first)
                time.sleep(SECOND_SECONDS)
                check("%s: the cleanup of the first signal waits" % name, proc.poll() is None,
                      "the script ended within %.1f s, so no second signal can arrive"
                      % SECOND_SECONDS)
                proc.send_signal(second)
                try:
                    status = proc.wait(timeout=GONE_SECONDS)
                except subprocess.TimeoutExpired:
                    status = None
                check("%s: the second signal ends the script at once" % name,
                      status == -second, "status %r after %d s:\n%s"
                      % (status, GONE_SECONDS, open(out).read()[-2000:]))
            finally:
                end_run(proc, [marker, suite, d])


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
    # The same kill, earlier: before the suite reaches its watch.
    early_case()
    # A cleanup that waits ends at once on a second signal, of either kind.
    second_signal_case(signal.SIGINT, signal.SIGTERM)
    second_signal_case(signal.SIGTERM, signal.SIGINT)
    # The real suite, so that a dropped call of the watch in its main() fails.
    suite_case()
    print("\n%d failed" % FAILED)
    return 0 if FAILED == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
