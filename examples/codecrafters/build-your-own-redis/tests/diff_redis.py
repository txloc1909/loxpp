#!/usr/bin/env python3
# diff_redis.py — differential test of the Lox++ Redis server against
# redis-server, an independent oracle.
#
# Each script is a list of commands. The test sends the same script to a
# fresh redis-server and to a fresh Lox++ server, and compares the replies one
# by one. Error replies compare by their first word only (for example "ERR"
# or "WRONGTYPE"): the text after it differs between Redis versions.
#
# Usage: tests/diff_redis.py [your_program.sh]

import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_stages import DEFAULT_PROG, Client, Error, Server, free_port  # noqa: E402

import subprocess  # noqa: E402
import socket  # noqa: E402
import time  # noqa: E402

PASS = 0
FAIL = 0

# One entry per group: (script name, [command, ...]). A command is a list of
# arguments.
SCRIPTS = {
    "base": [
        ("ping", [["PING"], ["ping"], ["PING", "hello"]]),
        ("echo", [["ECHO", "hey"], ["ECHO", ""], ["echo", "a b c"]]),
        ("set-get", [
            ["SET", "k", "v"], ["GET", "k"], ["GET", "missing"],
            ["SET", "k", "w"], ["GET", "k"],
            ["SET", "e", ""], ["GET", "e"],
        ]),
        ("set-expiry-options", [
            ["SET", "a", "1", "PX", "100000"], ["GET", "a"],
            ["SET", "b", "2", "EX", "100"], ["GET", "b"],
            ["SET", "c", "3", "px", "0"],
            ["SET", "d", "4", "PX", "-5"],
            ["SET", "f", "5", "PX", "abc"],
            ["SET", "g", "6", "PX"],
            ["SET", "h", "7", "BOGUS"],
        ]),
        ("arity", [["GET"], ["SET", "k"], ["ECHO"], ["ECHO", "a", "b"]]),
        ("unknown", [["NOSUCHCOMMAND"], ["PING"]]),
    ],
}


def normalize(reply):
    if isinstance(reply, Error):
        return ("error", reply.split(" ", 1)[0])
    if isinstance(reply, list):
        return [normalize(r) for r in reply]
    return reply


class RedisServer:
    def __init__(self):
        self.port = free_port()
        self.dir = tempfile.mkdtemp(prefix="loxpp-redis-oracle-")
        self.proc = subprocess.Popen(
            ["redis-server", "--port", str(self.port), "--bind", "127.0.0.1",
             "--save", "", "--appendonly", "no", "--dir", self.dir],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + 5
        self.up = False
        while time.time() < deadline:
            try:
                socket.create_connection(
                    ("127.0.0.1", self.port), timeout=0.2).close()
                self.up = True
                break
            except OSError:
                time.sleep(0.05)

    def client(self):
        return Client(self.port)

    def stop(self):
        self.proc.terminate()
        self.proc.wait(timeout=5)
        shutil.rmtree(self.dir, ignore_errors=True)


def run_script(server, script):
    c = server.client()
    try:
        return [normalize(c.call(*cmd)) for cmd in script]
    finally:
        c.close()


def main():
    global PASS, FAIL
    prog = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PROG)
    if shutil.which("redis-server") is None:
        print("redis-server not found on PATH")
        return 1
    for group, scripts in SCRIPTS.items():
        print("== %s" % group)
        for name, script in scripts:
            oracle = RedisServer()
            ours = Server(prog, [])
            try:
                if not oracle.up or not ours.up:
                    FAIL += 1
                    print("  FAIL %s  a server did not start (redis=%s lox=%s)"
                          % (name, oracle.up, ours.up))
                    continue
                want = run_script(oracle, script)
                got = run_script(ours, script)
                if want == got:
                    PASS += 1
                    print("  ok   %s" % name)
                else:
                    FAIL += 1
                    print("  FAIL %s" % name)
                    for cmd, w, g in zip(script, want, got):
                        if w != g:
                            print("       %r: redis %r, lox %r" % (cmd, w, g))
            finally:
                oracle.stop()
                ours.stop()
    print("\n%d passed, %d failed" % (PASS, FAIL))
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
