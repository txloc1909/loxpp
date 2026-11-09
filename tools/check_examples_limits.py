#!/usr/bin/env python3
"""Checks that tools/check_examples.py stops a run it cannot finish.

The tool ran each example with subprocess.run and no timeout, and it held the
whole of the example's stdout in memory as one string and then as one object
per line (issue #1176). One example that hangs therefore blocked the whole job
until the workflow's own limit and named no example, and one example on a
traced build — `sieve.lox` emits 370 MB in 15 s — was enough to take a 7 GB
python3 down with the kernel OOM killer.

Seventeen checks in twelve groups, each on a directory the test writes
itself, so no corpus is walked and the whole run costs a few seconds:

  1. a runner that runs one passing example passes;
  2. an example that never ends fails the run in bounded time, and the report
     names it and the limit it used;
  3. its own `// TIMEOUT: <seconds>` takes the place of --timeout, so a short
     directive binds a long flag;
  4. an example whose output passes --max-output-bytes fails by that limit and
     names it;
  5. an example that prints more than the report keeps is still matched, and a
     passing run prints no elision;
  6. a failing run with a long output elides the middle of the report;
  7. an unmatched CHECK fails the run and shows the output;
  8. a passing example passes and reports its check count, and a CHECK pair
     that arrives out of order fails;
  9. an example that halts fails by its exit code;
 10. `// CHECK:` matching ignores leading white space of the output line, as it
     always did;
 11. the `<name>.input` file still reaches the program's stdin;
 12. a directory with no .lox file is an error (exit 2), and --timeout or
     --max-output-bytes with no value is a usage error (exit 2).

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
        # says so with exit 2. Grading sixteen cases against it would read as
        # sixteen failures instead, so the first exit 2 stops this test with
        # the same code and message.
        d = write_dir(tmp, {"one.lox": '// CHECK: ok\nprint "ok";\n'})
        rc, out, _ = run([sys.executable, str(TOOL), native, str(d)])
        if rc == 2:
            print(out, file=sys.stderr, end="")
            sys.exit(2)
        check("the runner runs one example", rc == 0, out)

        # 1 and 2: a run that never ends, and its own limit.
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

        # 3: output past the limit, which is what a traced build emits.
        d = write_dir(tmp, {"flood.lox": FLOOD})
        rc, out, took = run(
            [sys.executable, str(TOOL), native, str(d), "--max-output-bytes", "1000"]
        )
        check(
            "output past --max-output-bytes fails by that limit",
            rc == 1 and "output over 1000 bytes" in out and took < 20,
            f"rc={rc} took={took:.1f}s out={out!r}",
        )

        # 4: more lines than the report keeps, still matched, middle elided.
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

        # 5: an unmatched CHECK names the line and shows output.
        d = write_dir(tmp, {"unmatched.lox": UNMATCHED})
        rc, out, _ = run([sys.executable, str(TOOL), native, str(d)])
        check(
            "an unmatched CHECK fails and shows the output",
            rc == 1 and "unmatched CHECK: 'nope'" in out and "something else" in out,
            out,
        )

        # 6: a passing example, with its checks in order.
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

        # 7: an exit code that is not zero.
        d = write_dir(tmp, {"halting.lox": HALTING})
        rc, out, _ = run([sys.executable, str(TOOL), native, str(d)])
        check(
            "an example that halts fails by its exit code",
            rc == 1 and "exit code" in out and "halting.lox" in out,
            out,
        )

        # 8: leading white space of the output line is ignored.
        d = write_dir(tmp, {"indented.lox": INDENTED})
        rc, out, _ = run([sys.executable, str(TOOL), native, str(d)])
        check("a CHECK matches a line that starts with spaces", rc == 0, out)

        # 9: the .input file still reaches stdin.
        d = write_dir(tmp, {"stdin.lox": STDIN, "stdin.input": "from the file\n"})
        rc, out, _ = run([sys.executable, str(TOOL), native, str(d)])
        check("the .input file reaches the program's stdin", rc == 0, out)

        # 10: a directory with no .lox file is an error.
        empty = tmp / "empty"
        empty.mkdir()
        rc, out, _ = run([sys.executable, str(TOOL), native, str(empty)])
        check("a directory with no example is exit 2", rc == 2, f"rc={rc} out={out!r}")

        # 11 and 12: an option with no value is a usage error.
        d = write_dir(tmp, {"passing.lox": PASSING})
        for option in ("--timeout", "--max-output-bytes"):
            rc, out, _ = run([sys.executable, str(TOOL), native, str(d), option])
            check(f"{option} with no value is exit 2", rc == 2, f"rc={rc} out={out!r}")

    if failures:
        print(f"\n{len(failures)} case(s) failed")
        sys.exit(1)
    print("\nall cases passed")


if __name__ == "__main__":
    main()
