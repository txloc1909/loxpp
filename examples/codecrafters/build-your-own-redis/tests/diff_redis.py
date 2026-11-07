#!/usr/bin/env python3
# diff_redis.py — differential test of the Lox++ Redis server against
# redis-server, an independent oracle.
#
# Each script is a list of commands. The test sends the same script to a
# fresh redis-server and to a fresh Lox++ server, and compares the replies one
# by one, error texts included. The oracle is the redis-server of the dev
# image (7.0.15); another version can word an error in another way.
#
# Usage: tests/diff_redis.py [your_program.sh]

import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_stages import DEFAULT_PROG, Client, Error, Server, free_port  # noqa: E402

import subprocess  # noqa: E402
import random  # noqa: E402
import socket  # noqa: E402
import time  # noqa: E402

PASS = 0
FAIL = 0

# One entry per group: (script name, [command, ...]). A command is a list of
# arguments.

def both_ends_script():
    """LPUSH, RPUSH, LPOP and LRANGE in a fixed random order, so that the
    elements cross between the two parts of the queue and the free slots
    are dropped many times."""
    rng = random.Random(1151)
    script = [["RPUSH", "q"] + [f"r{i}" for i in range(100)],
              ["LPOP", "q", "40"], ["LRANGE", "q", "0", "-1"]]
    n = 0
    for _ in range(400):
        op = rng.randrange(6)
        k = rng.randrange(1, 12)
        if op == 0:
            script.append(["LPUSH", "q"] + [f"l{n + i}" for i in range(k)])
            n += k
        elif op == 1:
            script.append(["RPUSH", "q"] + [f"r{n + i}" for i in range(k)])
            n += k
        elif op == 2:
            script.append(["LPOP", "q"])
        elif op == 3:
            script.append(["LPOP", "q", str(k)])
        elif op == 4:
            a, b = rng.randrange(-20, 20), rng.randrange(-20, 20)
            script.append(["LRANGE", "q", str(a), str(b)])
        else:
            script.append(["LLEN", "q"])
    script.append(["LRANGE", "q", "0", "-1"])
    return script


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
        ("set-options", [
            ["SET", "k", "1", "NX"], ["SET", "k", "2", "NX"], ["GET", "k"],
            ["SET", "k", "3", "XX"], ["SET", "m", "4", "XX"], ["GET", "m"],
            ["SET", "k", "5", "GET"], ["SET", "new", "6", "GET"],
            ["SET", "k", "7", "NX", "GET"], ["SET", "k", "8", "NX", "XX"],
            ["SET", "t", "1", "PX", "100000"], ["SET", "t", "2", "KEEPTTL"],
            ["GET", "t"], ["SET", "t", "3", "KEEPTTL", "PX", "10"],
            ["SET", "t", "4", "EX", "10", "PX", "100"],
            ["SET", "t", "5", "PX", "0100"], ["SET", "t", "6", "PX", "-0"],
            ["SET", "t", "7", "PX", "99999999999999"],
            ["SET", "t", "8", "EX", "9999999999999"],
            ["SET", "t", "9", "PX", "abc", "FOO"], ["SET", "t", "9", "PX", "0", "FOO"],
            ["SET", "t", "9", "PX", "abc"], ["SET", "t", "9", "PX", "0"],
        ]),
        ("set-absolute-expiry", [
            ["SET", "a", "1", "PXAT", "9999999999999"], ["GET", "a"],
            ["SET", "b", "2", "EXAT", "9999999999"], ["GET", "b"],
            ["SET", "c", "3", "PXAT", "1"], ["GET", "c"],
            ["SET", "d", "4", "EXAT", "1"], ["GET", "d"],
            ["SET", "e", "5", "PXAT", "0"], ["SET", "f", "6", "EXAT", "x"],
            ["SET", "g", "7", "PX", "10", "PXAT", "10"],
        ]),
        ("long-unknown-command", [["X" * 300], ["PING"]]),
        ("info-clients", [["INFO", "nosuchsection"]]),
        ("arity", [["GET"], ["SET", "k"], ["ECHO"], ["ECHO", "a", "b"],
                   ["PING", "a", "b"]]),
        ("unknown", [["NOSUCHCOMMAND"], ["NOSUCH", "a", "b"],
                     ["NOSUCH", "x" * 200, "y"], ["PING"]]),
    ],
    "lists": [
        ("push-and-range", [
            ["RPUSH", "l", "a", "b", "c"], ["LPUSH", "l", "z", "y"],
            ["LRANGE", "l", "0", "-1"], ["LRANGE", "l", "1", "2"],
            ["LRANGE", "l", "-3", "-2"], ["LRANGE", "l", "-100", "100"],
            ["LRANGE", "l", "4", "2"], ["LRANGE", "l", "10", "20"],
            ["LRANGE", "missing", "0", "-1"], ["LLEN", "l"], ["LLEN", "missing"],
        ]),
        ("pop", [
            ["RPUSH", "l", "1", "2", "3", "4"], ["LPOP", "l"], ["LPOP", "l", "2"],
            ["LPOP", "l", "0"], ["LPOP", "l", "5"], ["LPOP", "l"], ["LLEN", "l"],
            ["LPOP", "missing"], ["LPOP", "missing", "2"],
        ]),
        ("both-ends", both_ends_script()),
        ("blpop-no-wait", [
            ["RPUSH", "b", "x", "y"], ["BLPOP", "missing", "b", "0"],
            ["BLPOP", "b", "0.01"], ["BLPOP", "b", "0.01"],
        ]),
        ("errors", [
            ["SET", "s", "v"], ["RPUSH", "s", "x"], ["LPUSH", "s", "x"],
            ["LLEN", "s"], ["LRANGE", "s", "0", "1"], ["LPOP", "s"],
            ["BLPOP", "s", "0"], ["RPUSH", "l", "x"], ["GET", "l"],
            ["LRANGE", "l", "a", "1"], ["LPOP", "l", "-1"], ["LPOP", "l", "x"],
            ["BLPOP", "l", "-1"], ["BLPOP", "l", "abc"], ["RPUSH", "l"],
            ["LLEN"], ["LRANGE", "l", "0"],
        ]),
    ],
}


def normalize(reply):
    if isinstance(reply, Error):
        return ("error", str(reply))
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
                try:
                    got = run_script(ours, script)
                except (OSError, EOFError, ValueError) as e:
                    FAIL += 1
                    print("  FAIL %s  the Lox++ server failed: %r" % (name, e))
                    print("       server log:\n%s" % ours.log_text())
                    continue
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
