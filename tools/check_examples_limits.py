#!/usr/bin/env python3
"""Checks that tools/check_examples.py stops a run it cannot finish.

The tool ran each example with subprocess.run and no timeout, and it held the
whole of the example's stdout in memory as one string and then as one object
per line (issue #1176). One example that hangs therefore blocked the whole job
until the workflow's own limit and named no example, and one example on a
traced build — `sieve.lox` emits 370 MB in 15 s — was enough to take a 7 GB
python3 down with the kernel OOM killer.

Every group runs on a directory the test writes itself, so no corpus is
walked and the whole run costs a few seconds. This file is the list: every
`check(...)` call is a case, each prints one `ok` line, and no count is written
here by hand, because a hand-maintained count goes stale and a hand-maintained
list leaves the cases it forgot looking covered.

What the cases cover: an example that never ends; the time limit and its
directive in every spelling it must refuse; the byte limit on a long line and
on a stream that never ends its line; the line bound itself, which only the
matcher can show; a character a read boundary splits; the report's elision and
its truncation marker; CHECK order and white space; the `.input` file; and the
usage errors.

`stop()` has a case of its own: an example that hangs is stopped at its limit,
through a runner that leaves a descendant, and the whole run must come back soon
after. A runner that traps SIGTERM is stopped by the SIGKILL round, so its case
needs that round. Waiting for such a group costs the full limit of each signal, because the
stopped descendant stays in the group as a zombie and only a parent that reaps
can remove it.

Usage:
    tools/check_examples_limits.py --native <loxpp-binary>
"""

import argparse
import pathlib
import subprocess
import sys
import tempfile
import time

from runner_precondition import require_runner

ROOT = pathlib.Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "check_examples.py"

# A run that does not end, written as a long sleep rather than a spin: a build
# that traces each instruction would print its way past --max-output-bytes
# before the timeout could fire, and the case would then depend on the build.
HANG = """// CHECK: never
sleep(60);
"""

HANG_DIRECTIVE = """// TIMEOUT: 1
// CHECK: never
sleep(60);
"""

FLOOD = """// CHECK: done
var i = 0;
while (i < 200000) { print "flood-flood-flood-flood"; i = i + 1; }
print "done";
"""

MANY_LINES = """// CHECK: the last line
var i = 0;
while (i < 500) { print "line " + str(i); i = i + 1; }
print "the last line";
"""

UNMATCHED = """// CHECK: nope
print "something else";
"""

PASSING = """// CHECK: one
// CHECK: three
print "one";
print "two";
print "three";
"""

HALTING = """// CHECK: before
print "before";
throw "boom";
"""

INDENTED = """// CHECK: indented
print "   indented";
"""

STDIN = """// CHECK: from the file
print input();
"""

failures: list[str] = []


def check(name: str, ok: bool, detail: str = "") -> None:
    if ok:
        print(f"ok   {name}")
    else:
        print(f"FAIL {name}  {detail}")
        failures.append(name)


def run(args: list[str], timeout: float = 60.0) -> tuple[int, str, float]:
    started = time.monotonic()
    proc = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    return proc.returncode, proc.stdout + proc.stderr, time.monotonic() - started


def write_dir(tmp: pathlib.Path, files: dict[str, str]) -> pathlib.Path:
    d = tmp / f"case{len(list(tmp.iterdir()))}"
    d.mkdir()
    for name, text in files.items():
        (d / name).write_text(text, encoding="utf-8")
    return d


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    args = parser.parse_args()
    native = args.native
    require_runner(native)

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = pathlib.Path(tmpdir)

        # A runner that cannot run is a setup mistake, and the tool under test
        # says so with exit 2. Grading the rest of the cases against it would
        # read as that many failures instead, so the first exit 2 stops this
        # test with the same code and message.
        d = write_dir(tmp, {"one.lox": '// CHECK: ok\nprint "ok";\n'})
        rc, out, _ = run([sys.executable, str(TOOL), native, str(d)])
        if rc == 2:
            print(out, file=sys.stderr, end="")
            sys.exit(2)
        check("the runner runs one example", rc == 0, out)

        # A run that never ends, and its own limit.
        d = write_dir(tmp, {"hang.lox": HANG})
        rc, out, took = run([sys.executable, str(TOOL), native, str(d), "--timeout", "1"])
        check(
            "a hanging example fails in bounded time",
            rc == 1 and "TIMEOUT  hang.lox" in out and took < 20,
            f"rc={rc} took={took:.1f}s out={out!r}",
        )
        check(
            "the report names the limit it used",
            "(killed after 1s)" in out,
            out,
        )

        d = write_dir(tmp, {"hang_directive.lox": HANG_DIRECTIVE})
        rc, out, took = run([sys.executable, str(TOOL), native, str(d), "--timeout", "30"])
        check(
            "an example's own TIMEOUT takes the place of --timeout",
            rc == 1 and "killed after 1s" in out and took < 20,
            f"rc={rc} took={took:.1f}s out={out!r}",
        )

        # Output past the limit, which is what a traced build emits.
        d = write_dir(tmp, {"flood.lox": FLOOD})
        rc, out, took = run(
            [sys.executable, str(TOOL), native, str(d), "--max-output-bytes", "1000"]
        )
        check(
            "output past --max-output-bytes fails by that limit",
            rc == 1 and "output over 1000 bytes" in out and took < 20,
            f"rc={rc} took={took:.1f}s out={out!r}",
        )

        # More lines than the report keeps: matched, and the middle elided.
        d = write_dir(tmp, {"many.lox": MANY_LINES})
        rc, out, _ = run([sys.executable, str(TOOL), native, str(d)])
        check("a long output is still matched", rc == 0 and "PASS  many.lox" in out, out)
        check("a passing run prints no elision", "..." not in out, out)

        # The CHECK itself must not appear, so that the run fails and the
        # report has to show a head and a tail of a long output.
        (d / "many.lox").write_text(
            MANY_LINES.replace("// CHECK: the last line", "// CHECK: absent line")
        )
        rc, out, _ = run([sys.executable, str(TOOL), native, str(d)])
        check(
            "a failure report elides the middle",
            rc == 1 and "unmatched CHECK" in out and "'...'" in out,
            out,
        )

        # An unmatched CHECK names the line and shows output.
        d = write_dir(tmp, {"unmatched.lox": UNMATCHED})
        rc, out, _ = run([sys.executable, str(TOOL), native, str(d)])
        check(
            "an unmatched CHECK fails and shows the output",
            rc == 1 and "unmatched CHECK: 'nope'" in out and "something else" in out,
            out,
        )

        # A passing example, with its checks in order.
        d = write_dir(tmp, {"passing.lox": PASSING})
        rc, out, _ = run([sys.executable, str(TOOL), native, str(d)])
        check(
            "a passing example reports its check count",
            rc == 0 and "PASS  passing.lox  (2 checks)" in out,
            out,
        )

        d = write_dir(
            tmp,
            {"out_of_order.lox": PASSING.replace("// CHECK: one\n// CHECK: three",
                                                "// CHECK: three\n// CHECK: one")},
        )
        rc, out, _ = run([sys.executable, str(TOOL), native, str(d)])
        check(
            "the CHECK lines must match in order",
            rc == 1 and "unmatched CHECK: 'one'" in out,
            out,
        )

        # An exit code that is not zero.
        d = write_dir(tmp, {"halting.lox": HALTING})
        rc, out, _ = run([sys.executable, str(TOOL), native, str(d)])
        check(
            "an example that halts fails by its exit code",
            rc == 1 and "exit code" in out and "halting.lox" in out,
            out,
        )

        # Leading white space of the output line is ignored.
        d = write_dir(tmp, {"indented.lox": INDENTED})
        rc, out, _ = run([sys.executable, str(TOOL), native, str(d)])
        check("a CHECK matches a line that starts with spaces", rc == 0, out)

        # The .input file still reaches stdin.
        d = write_dir(tmp, {"stdin.lox": STDIN, "stdin.input": "from the file\n"})
        rc, out, _ = run([sys.executable, str(TOOL), native, str(d)])
        check("the .input file reaches the program's stdin", rc == 0, out)

        # A two-byte character that a 64 KiB read boundary splits.
        long_line = "x" * 65535 + "\u00e9"
        d = write_dir(tmp, {"split.lox": f'// CHECK: {long_line}\nprint "{long_line}";\n'})
        rc, out, _ = run([sys.executable, str(TOOL), native, str(d)])
        check("a character split by a read boundary still matches", rc == 0, out[:400])

        # One line longer than the limit, so the limit cannot wait for a line
        # end.
        d = write_dir(
            tmp,
            {
                "oneline.lox": '// CHECK: done\n'
                'var s = "";\n'
                'while (len(s) < 100000) { s = s + "0123456789abcdef"; }\n'
                'print s;\n'
            },
        )
        rc, out, _ = run(
            [sys.executable, str(TOOL), native, str(d), "--max-output-bytes", "1000"]
        )
        check(
            "a single line past the limit fails by the limit",
            rc == 1 and "output over 1000 bytes" in out,
            out[:400],
        )

        # A stream that never ends its line. A stub runner writes 100 KB of one
        # line and then sleeps, so the only way to reach the limit is to count
        # the bytes as they arrive.
        stub = tmp / "stub.sh"
        stub.write_text(
            "#!/bin/sh\ndd if=/dev/zero bs=1000 count=100 2>/dev/null | tr '\\0' x\nsleep 30\n",
            encoding="utf-8",
        )
        stub.chmod(0o755)
        d = write_dir(tmp, {"never.lox": "// CHECK: done\n"})
        rc, out, took = run(
            [
                sys.executable,
                str(TOOL),
                str(stub),
                str(d),
                "--max-output-bytes",
                "1000",
                "--timeout",
                "20",
            ]
        )
        check(
            "a stream that never ends its line fails by the limit, not the timeout",
            rc == 1 and "output over 1000 bytes" in out and took < 15,
            f"rc={rc} took={took:.1f}s out={out[:300]!r}",
        )

        # A long line that does not match is shortened in the report. This is
        # the report's own bound, not the matcher's; the matcher's is below,
        # where the tool is imported.
        d = write_dir(
            tmp,
            {
                "longline.lox": '// CHECK: absent\n'
                'var s = "";\n'
                'while (len(s) < 100000) { s = s + "0123456789abcdef"; }\n'
                'print s;\n'
            },
        )
        rc, out, _ = run([sys.executable, str(TOOL), native, str(d)])
        longest = max((len(line) for line in out.splitlines()), default=0)
        check(
            "a long line is shortened in the report, not quoted whole",
            rc == 1 and longest < 400,
            f"rc={rc} longest_report_line={longest}",
        )

        # The truncation marker: a line of exactly REPORT_LINE_CHARS is shown
        # whole, and one character more gets the marker.
        stub200 = tmp / "stub200.sh"
        stub200.write_text(
            "#!/bin/sh\nprintf '%0.sy' $(seq 1 200)\nprintf '\\n'\n",
            encoding="utf-8",
        )
        stub200.chmod(0o755)
        stub201 = tmp / "stub201.sh"
        stub201.write_text(
            "#!/bin/sh\nprintf '%0.sy' $(seq 1 201)\nprintf '\\n'\n",
            encoding="utf-8",
        )
        stub201.chmod(0o755)
        d = write_dir(tmp, {"marker.lox": "// CHECK: absent\n"})
        rc, out200, _ = run([sys.executable, str(TOOL), str(stub200), str(d)])
        rc2, out201, _ = run([sys.executable, str(TOOL), str(stub201), str(d)])
        shown200 = [l for l in out200.splitlines() if l.strip().startswith("'")]
        shown201 = [l for l in out201.splitlines() if l.strip().startswith("'")]
        check(
            "a line of exactly the report width gets no marker",
            rc == 1 and shown200 and "..." not in shown200[0],
            f"rc={rc} shown={shown200[:1]}",
        )
        check(
            "one character more gets the marker",
            rc2 == 1 and shown201 and shown201[0].rstrip("'").endswith("..."),
            f"rc={rc2} shown={shown201[:1]}",
        )

        # `stop()` must return soon after the limit ends the example. Waiting
        # for a process group that is already gone costs two seconds per run.
        d = write_dir(tmp, {"slowstop.lox": "// CHECK: never\nsleep(60);\n"})
        slow = tmp / "slowstop.sh"
        slow.write_text("#!/bin/sh\nsleep 300 &\nwait\n", encoding="utf-8")
        slow.chmod(0o755)
        rc, out, took = run(
            [sys.executable, str(TOOL), str(slow), str(d), "--timeout", "0.5"]
        )
        check(
            "a stopped run returns soon after its limit",
            rc == 1 and "TIMEOUT" in out and took < 1.5,
            f"rc={rc} took={took:.2f}s",
        )

        # A runner that traps SIGTERM. Only the SIGKILL round ends it. If stop()
        # sent SIGTERM alone, the tool would wait for the runner for ever, so
        # this run has a limit of its own, and a tool that does not return
        # within it fails the case.
        d = write_dir(tmp, {"trapped.lox": "// CHECK: never\nsleep(60);\n"})
        trapped = tmp / "trapped.sh"
        trapped.write_text('#!/bin/sh\ntrap "" TERM\nsleep 60 &\nwait\n', encoding="utf-8")
        trapped.chmod(0o755)
        try:
            rc, out, took = run(
                [sys.executable, str(TOOL), str(trapped), str(d), "--timeout", "0.5"],
                timeout=20,
            )
        except subprocess.TimeoutExpired:
            rc, out, took = None, "the tool did not return", 20.0
        check(
            "a runner that traps SIGTERM is stopped by SIGKILL in time",
            rc == 1 and "TIMEOUT" in out and took < 5,
            f"rc={rc} took={took:.2f}s out={out[:200]!r}",
        )

        # A directory with no .lox file is an error.
        empty = tmp / "empty"
        empty.mkdir()
        rc, out, _ = run([sys.executable, str(TOOL), native, str(empty)])
        check("a directory with no example is exit 2", rc == 2, f"rc={rc} out={out!r}")

        # An option with no value is a usage error.
        d = write_dir(tmp, {"passing.lox": PASSING})
        for option in ("--timeout", "--max-output-bytes"):
            rc, out, _ = run([sys.executable, str(TOOL), native, str(d), option])
            check(f"{option} with no value is exit 2", rc == 2, f"rc={rc} out={out!r}")

        rc, out, _ = run(
            [sys.executable, str(TOOL), native, str(d), "--max-output-bytes", "0"]
        )
        check("--max-output-bytes 0 is exit 2, not 'no limit'", rc == 2, f"rc={rc} out={out!r}")

        d = write_dir(tmp, {"zero.lox": "// TIMEOUT: 0\n// CHECK: never\nprint \"x\";\n"})
        rc, out, _ = run([sys.executable, str(TOOL), native, str(d)])
        check("// TIMEOUT: 0 is exit 2", rc == 2 and "greater than zero" in out, f"rc={rc} out={out!r}")

        for bad in ("", "-1", "nan", "1e10", "5s"):
            d = write_dir(
                tmp,
                {
                    "bad.lox": f'// TIMEOUT:{bad}\n// CHECK: never\nprint "x";\n'
                    if bad
                    else '// TIMEOUT:\n// CHECK: never\nprint "x";\n'
                },
            )
            rc, out, _ = run([sys.executable, str(TOOL), native, str(d)])
            check(
                f"// TIMEOUT: {bad!r} is exit 2", rc == 2, f"rc={rc} out={out[:200]!r}"
            )

        # A fresh directory: the one above carries a bad directive, which would
        # answer for the flag and leave its rule untested.
        d_flag = write_dir(tmp, {"plain.lox": PASSING})
        for bad in ("nan", "1e10", "-1", "0", "inf"):
            rc, out, _ = run(
                [sys.executable, str(TOOL), native, str(d_flag), "--timeout", bad]
            )
            check(
                f"--timeout {bad} is exit 2, not a traceback",
                rc == 2 and "Traceback" not in out,
                f"rc={rc} out={out!r}",
            )
        rc, out, _ = run(
            [sys.executable, str(TOOL), native, str(d_flag), "--timeout", "5"]
        )
        check("--timeout with a good value still runs", rc == 0, f"rc={rc} out={out!r}")
        rc, out, _ = run(
            [sys.executable, str(TOOL), native, str(d_flag), "--timeout", "86400"]
        )
        check("--timeout at the ceiling still runs", rc == 0, f"rc={rc} out={out[:200]!r}")
        rc, out, _ = run(
            [sys.executable, str(TOOL), native, str(d_flag), "--timeout", "86401"]
        )
        check("--timeout one second over the ceiling is exit 2", rc == 2, f"rc={rc} out={out[:200]!r}")

    # The line bound itself: the part kept while a line accumulates, and the
    # text kept for the report, are both bounded. A mutant that assembles whole
    # lines fails these, and no program can be written to show it.
    sys.path.insert(0, str(ROOT / "tools"))
    import check_examples as tool_module

    capture = tool_module.Capture(["ab"])
    capture.feed("z" * 10_000_000)
    check(
        "the match buffer is bounded by the longest CHECK",
        capture.kept <= len("ab") + 1 and capture.overflow,
        f"kept={capture.kept} overflow={capture.overflow}",
    )
    check(
        "the report text is bounded too",
        capture.shown_len <= tool_module.REPORT_LINE_CHARS,
        f"shown={capture.shown_len}",
    )
    capture.finish()
    check(
        "a line longer than the longest CHECK cannot match",
        capture.missing() == "ab",
        f"missing={capture.missing()!r}",
    )
    # The bound must not hand a prefix of a long line to the matcher: "ab x"
    # kept to one character past "ab" is "ab ", and stripping it would pass a
    # program whose line is not "ab" at all.
    prefix = tool_module.Capture(["ab"])
    prefix.feed("ab x\n")
    prefix.finish()
    check(
        "a truncated line does not match the CHECK it starts with",
        prefix.missing() == "ab",
        f"missing={prefix.missing()!r} overflow={prefix.overflow}",
    )
    # A line that starts with the CHECK text and goes on is not that CHECK. It
    # is short enough to stay in the buffer, so only the exact comparison can
    # reject it: a matcher that tests the prefix passes it.
    extended = tool_module.Capture(["ab"])
    extended.feed("abc\n")
    extended.finish()
    check(
        "a line that starts with the CHECK text and goes on does not match",
        extended.missing() == "ab" and not extended.overflow,
        f"missing={extended.missing()!r} overflow={extended.overflow}",
    )
    # The same rule end to end: the program prints "abc", the example checks
    # for "ab", and the run must fail.
    with tempfile.TemporaryDirectory() as prefix_dir:
        d = write_dir(pathlib.Path(prefix_dir), {"prefix.lox": '// CHECK: ab\nprint "abc";\n'})
        rc, out, _ = run([sys.executable, str(TOOL), native, str(d)])
    check(
        "an example whose output starts with the CHECK text and goes on fails",
        rc == 1 and "prefix.lox" in out,
        f"rc={rc} out={out[:300]!r}",
    )
    # The same guard on the piece after the buffer is full: a line that fills
    # the buffer with the CHECK and white space, then continues with a real
    # character, is not the CHECK either.
    late = tool_module.Capture(["ab"])
    late.feed("ab" + " " * 100)
    late.feed("x")
    late.finish()
    check(
        "a line that goes on after the buffer is full cannot match",
        late.missing() == "ab",
        f"missing={late.missing()!r} overflow={late.overflow}",
    )
    trailing = tool_module.Capture(["ab"])
    trailing.feed("ab")
    trailing.feed(" " * 1)          # fills the buffer
    trailing.feed(" " * 1000)       # and goes on, in white space only
    trailing.finish()
    check(
        "white space past the buffer is not a mismatch",
        trailing.missing() is None,
        f"missing={trailing.missing()!r} overflow={trailing.overflow}",
    )
    matching = tool_module.Capture(["ab"])
    matching.feed("  ab  \n")
    matching.finish()
    check("a matching line still matches after trimming", matching.missing() is None)

    if failures:
        print(f"\n{len(failures)} case(s) failed")
        sys.exit(1)
    print("\nall cases passed")


if __name__ == "__main__":
    main()
