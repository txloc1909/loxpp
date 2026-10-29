#!/usr/bin/env python3
"""Checks that benchmarks/run.py compares every backend with a pinned value.

A benchmark that prints the same wrong checksum on every backend must fail the
run, and a program with no pinned value must not join the suite. This checker
runs run.py's main() against stub backends that print a chosen checksum, so it
needs no built binary, and asserts the exit code of each case:

  correct      every backend prints the pinned value                 exit 0
  shared-wrong every backend prints one wrong value                  exit 1
  one-wrong    one backend prints a wrong value                      exit 1
  disagree     backends print two values, one of them the pinned one exit 1
  missing      a program has no pinned value; no launch happens      exit 2
  no-backend   a backend cannot run                                  exit 1
  float        a float value inside and outside the tolerance        exit 0, 1
  latency-*    the same shared-wrong and missing cases for --latency

It also checks that expected.py names exactly the programs generate.py builds
and the files under benchmarks/latency/.

Usage:
    tools/check_benchmark_expected.py

Exit status: 0 when every case holds, 1 when a case fails.
"""

import contextlib
import importlib.util
import io
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BENCH = REPO_ROOT / "benchmarks"

STUB = '''\
import sys
backend, prog = sys.argv[1], sys.argv[2]
open(sys.argv[3], "a").write(backend + " " + prog + "\\n")
for line in open(prog):
    if line.startswith(backend + "="):
        value = line.split("=", 1)[1].strip()
        break
else:
    sys.exit(7)
if "latency" in prog:
    print("LATENCY 10 10 1.0 2.0 3.0 " + value)
else:
    print("HARNESS 0 1000.0 " + value)
'''


def load_run():
    sys.path.insert(0, str(BENCH))
    spec = importlib.util.spec_from_file_location("bench_run", BENCH / "run.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check_tables(run, failures):
    sys.path.insert(0, str(BENCH))
    import generate
    import expected
    want = set(generate.CONFIG)
    have = set(expected.EXPECTED)
    if want != have:
        failures.append(f"expected.py and generate.CONFIG differ: "
                        f"only in CONFIG {sorted(want - have)}, "
                        f"only in expected.py {sorted(have - want)}")
    lat = {p.stem for p in (BENCH / "latency").glob("*.lox")}
    if lat != set(expected.LATENCY_EXPECTED):
        failures.append(f"LATENCY_EXPECTED and latency/ differ: {sorted(lat)}")
    for table in (expected.EXPECTED, expected.LATENCY_EXPECTED):
        for name, row in table.items():
            if len(row) != 4 or not all(row[2:]):
                failures.append(f"{name}: entry needs a kind and a source")


def invoke(run, argv):
    out, err = io.StringIO(), io.StringIO()
    code = 0
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            run.main(argv)
        except SystemExit as e:
            code = e.code if isinstance(e.code, int) else 1
        except Exception as e:  # a crash is a failed case, not a pass
            code = 99
            err.write(f"crash: {e!r}\n")
    return code, out.getvalue() + err.getvalue()


def main():
    run = load_run()
    failures = []
    check_tables(run, failures)

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        stub = tmp / "stub.py"
        stub.write_text(STUB)
        log = tmp / "launches.log"
        progs, lats = tmp / "programs", tmp / "latency"
        progs.mkdir()
        lats.mkdir()
        runner = tmp / "runner"
        runner.write_text("")
        runner.chmod(0o755)

        run.PROGRAMS, run.LATENCY, run.PIN = progs, lats, []
        run.BACKENDS = {b: (lambda p, b=b: [sys.executable, str(stub), b, p,
                                            str(log)])
                        for b in ("a", "b")}
        run.BACKENDS["gone"] = lambda p: [str(tmp / "no-such-binary"), p]
        run.CLOCK_KIND = {"a": "cpu", "b": "cpu", "gone": "cpu"}
        run.RUNNER = {"a": runner, "b": runner, "gone": runner}
        run.EXPECTED = {"prog": ("42", False, "math", "stub"),
                        "fl": ("1.5", True, "math", "stub")}
        run.LATENCY_EXPECTED = {"lat": ("42", False, "math", "stub")}

        def case(label, directory, name, lines, argv, code, text=None,
                 launches=None):
            for old in directory.glob("*.lox"):
                old.unlink()
            (directory / f"{name}.lox").write_text(lines)
            log.write_text("")
            got, output = invoke(run, argv)
            n = len(log.read_text().splitlines())
            if got != code:
                failures.append(f"{label}: exit {got}, want {code}\n{output}")
            elif text and text not in output:
                failures.append(f"{label}: output lacks {text!r}\n{output}")
            elif launches is not None and n != launches:
                failures.append(f"{label}: {n} launches, want {launches}")

        base = ["--procs", "1", "--backends", "a", "b"]
        lbase = base + ["--latency"]
        case("correct", progs, "prog", "a=42\nb=42\n", base, 0)
        case("shared-wrong", progs, "prog", "a=41\nb=41\n", base, 1,
             "differs from the pinned value")
        case("one-wrong", progs, "prog", "a=42\nb=41\n", base, 1, "[b]")
        case("disagree", progs, "prog", "a=42\nb=43\n", base, 1)
        case("missing", progs, "other", "a=42\nb=42\n", base, 2,
             "no pinned checksum", launches=0)
        case("no-backend", progs, "prog", "a=42\n",
             ["--procs", "1", "--backends", "a", "gone"], 1, "did not run")
        case("float-in", progs, "fl", "a=1.5000000001\nb=1.5\n", base, 0)
        case("float-out", progs, "fl", "a=1.6\nb=1.6\n", base, 1)
        case("latency-correct", lats, "lat", "a=42\nb=42\n", lbase, 0)
        case("latency-shared-wrong", lats, "lat", "a=41\nb=41\n", lbase, 1,
             "differs from the pinned value")
        case("latency-missing", lats, "other", "a=42\nb=42\n", lbase, 2,
             "no pinned checksum", launches=0)

    for f in failures:
        print(f"FAIL {f}")
    if failures:
        return 1
    print("benchmark expected-checksum cases hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
