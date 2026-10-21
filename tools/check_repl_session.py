#!/usr/bin/env python3
"""Runs REPL sessions on one loxpp binary and checks their output.

A REPL session is one program entered one line at a time
(spec/04-semantics.md, A Session). The C++ tests in test/test_repl.cpp drive a
VM line by line; this checker drives the real REPL, so it also covers the
tool's own wiring (src/main.cpp: session mode, and the end of the session).

Each session is a list of lines, fed to stdin of the binary, and the stdout
lines it must produce, in order. Every expected line must appear in that
order, and a line may sit after other output: a debug build writes a trace of
every instruction to stdout, and the deadlock report goes to stderr.

Usage: tools/check_repl_session.py <loxpp binary>

Exit status: 0 when every session passes, 1 when one fails, 2 on a usage or
timeout error.
"""

import subprocess
import sys

# A timeout that a session of this size never reaches on a slow host, but that
# catches a hang: a session that the runtime does not end at the last line
# waits forever for another line.
TIMEOUT_SECONDS = 120

SESSIONS = [
    (
        # A worker of a later line defines the declarations of the earlier
        # lines, including the constructor tags, and answers the main task.
        "worker defines an earlier line",
        [
            "enum Opt { Some(v) None }",
            "fun twice(x) { return x * 2; }",
            "fun entry(main) { task.send(main, Opt.Some(twice(3))); }",
            "task.spawn_worker(entry, task.self());",
            "var got = task.receive();",
            "var r = match got { case Opt.Some(v) => v case Opt.None => 0 };",
            "print r;",
        ],
        ["6"],
    ),
    (
        # A worker waits for a message that no line sent yet, so it must
        # outlive the line that spawned it.
        "worker outlives its line",
        [
            "fun entry(main) { var m = task.receive(); "
            "task.send(main, m + 1); }",
            "var w = task.spawn_worker(entry, task.self());",
            "task.send(w, 41);",
            "var got = task.receive();",
            "print got;",
        ],
        ["42"],
    ),
    (
        # A deadlock report ends the line, not the session: the line after it
        # still reaches the worker and receives its reply.
        "session goes on after a deadlock",
        [
            "fun entry(main) { var m = task.receive(); "
            "task.send(main, m + 1); }",
            "var w = task.spawn_worker(entry, task.self());",
            "var lost = task.receive();",
            "task.send(w, 5);",
            "var got = task.receive();",
            "print got;",
        ],
        ["6"],
    ),
    (
        # A declaration of an earlier line applies to the top-level code of a
        # later line, and the process exits after the last line.
        "declaration carries over",
        [
            "enum Opt { Some(v) None }",
            "print Opt.Some(1);",
        ],
        ["Opt::Some(1)"],
    ),
]


def run_session(binary, lines):
    return subprocess.run(
        [binary],
        input="".join(line + "\n" for line in lines),
        capture_output=True,
        text=True,
        timeout=TIMEOUT_SECONDS,
    )


def check(name, lines, expected, binary):
    try:
        result = run_session(binary, lines)
    except subprocess.TimeoutExpired:
        print("FAIL %s: the session did not end" % name)
        return False
    if result.returncode != 0:
        print(
            "FAIL %s: exit status %d\n%s"
            % (name, result.returncode, result.stderr)
        )
        return False
    output = [line.rstrip() for line in result.stdout.splitlines()]
    at = 0
    for want in expected:
        while at < len(output) and output[at] != want:
            at += 1
        if at == len(output):
            print(
                "FAIL %s: stdout has no line %r\n--- stdout:\n%s\n"
                "--- stderr:\n%s"
                % (name, want, result.stdout, result.stderr)
            )
            return False
        at += 1
    print("PASS %s" % name)
    return True


def main():
    if len(sys.argv) != 2:
        print("usage: check_repl_session.py <loxpp binary>", file=sys.stderr)
        return 2
    binary = sys.argv[1]
    ok = True
    for name, lines, expected in SESSIONS:
        ok = check(name, lines, expected, binary) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
