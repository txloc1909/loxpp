#!/usr/bin/env python3
"""
Differential check: bootstrap's operator-overloading dispatch against
native's (issue #474).

The bootstrap interpreter now dispatches the same dunder methods the native
VM and JVM do (__add__ __sub__ __mul__ __div__ __mod__ __neg__ __lt__ __gt__
__eq__ __contains__ __call__ __index_get__ __index_set__ __iter__ __len__
__slice__ __hash__ __str__), and supports callMethod on a closure-backed
method (issue #496).
This runs a small corpus of programs that exercise each method through both
consumers and asserts byte-identical stdout.

Native's stdout is the ground truth. A case whose native run fails (non-zero
exit) is a corpus bug and is reported, not skipped.

A release build is required: a debug build's instruction trace floods native's
stdout with a disassembly of bootstrap/loxpp_interpreter.lox itself (native is
the interpreter that runs the bootstrap script). This follows the same
--trace-enabled / LOXPP_BOOTSTRAP_TRACE_ENABLED skip rule as the other
bootstrap ctest entries.

Usage:
    tools/check_bootstrap_operator_overload.py [--native PATH]
        [--bootstrap PATH] [--timeout SECONDS]

Exits 0 when every case's bootstrap stdout matches native's byte for byte.
Exits 1 and prints every mismatch otherwise. Exits 125 (ctest's
SKIP_RETURN_CODE) when --trace-enabled says the binary under test was built
with the debug trace macros on.
"""

import argparse
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

SKIP_RETURN_CODE = 125


@dataclass
class Case:
    name: str
    program: str  # full source text, including trailing newline


# One case per operator family, each a complete program whose output is
# deterministic on both consumers.
CASES = [
    Case(
        "arithmetic",
        "class V {\n"
        "  init(x) { this.x = x; }\n"
        "  __add__(o) { return this.x + o.x; }\n"
        "  __sub__(o) { return this.x - o.x; }\n"
        "  __mul__(o) { return this.x * o.x; }\n"
        "  __div__(o) { return this.x / o.x; }\n"
        "  __mod__(o) { return this.x % o.x; }\n"
        "  __neg__() { return -this.x; }\n"
        "}\n"
        "print V(3) + V(4);\n"
        "print V(3) - V(4);\n"
        "print V(3) * V(4);\n"
        "print V(3) / V(4);\n"
        "print V(3) % V(4);\n"
        "print -V(3);\n",
    ),
    Case(
        "comparison_equality",
        "class V {\n"
        "  init(x) { this.x = x; }\n"
        "  __lt__(o) { return this.x < o.x; }\n"
        "  __gt__(o) { return this.x > o.x; }\n"
        "  __eq__(o) { return this.x == o.x; }\n"
        "}\n"
        "var a = V(3);\n"
        "var b = V(4);\n"
        "print a < b;\n"
        "print a > b;\n"
        "print a == b;\n"
        "print a != b;\n"
        "print a <= b;\n"
        "print a >= b;\n",
    ),
    Case(
        "contains_call",
        "class Box {\n"
        "  init(x) { this.x = x; }\n"
        "  __contains__(v) { return v == this.x; }\n"
        "  __call__(n) { return this.x + n; }\n"
        "}\n"
        "var box = Box(10);\n"
        "print 10 in box;\n"
        "print 3 in box;\n"
        "print box(5);\n",
    ),
    Case(
        "index",
        "class T {\n"
        "  init() { this.v = 0; }\n"
        "  __index_get__(k) { return this.v + k; }\n"
        "  __index_set__(k, v) { this.v = v; return 999; }\n"
        "}\n"
        "var t = T();\n"
        "print t[10];\n"
        "print (t[\"k\"] = 7);\n"
        "print t[0];\n",
    ),
    Case(
        # The result depends on the argument order, so a swapped start/end is
        # visible: 3*100 + 4 is not 4*100 + 3.
        "slice",
        "class S {\n"
        "  init(n) { this.n = n; }\n"
        "  __slice__(start, end) { return this.n + start * 100 + end; }\n"
        "}\n"
        "print S(1000)[3:4];\n"
        "print [10, 20, 30, 40][1:3];\n"
        "print \"hello\"[1:3];\n",
    ),
    Case(
        "iter",
        "class R {\n"
        "  init(n) { this.n = n; }\n"
        "  __iter__() {\n"
        "    var o = [];\n"
        "    var i = 0;\n"
        "    while (i < this.n) { o.append(i); i = i + 1; }\n"
        "    return o;\n"
        "  }\n"
        "}\n"
        "var sum = 0;\n"
        "for (var x in R(4)) { sum = sum + x; }\n"
        "print sum;\n",
    ),
    Case(
        "length",
        "class L {\n"
        "  init(n) { this.n = n; }\n"
        "  __len__() { return this.n; }\n"
        "}\n"
        "print len([1, 2, 3]);\n"
        "print len(\"hello\");\n"
        "print len({\"a\": 1, \"b\": 2});\n"
        "print len(L(7));\n"
        "print len(\"ab\") + 1;\n"
        "print -len([1]);\n",
    ),
    Case(
        "length_inherited",
        "class A { __len__() { return 4; } }\n"
        "class B < A {}\n"
        "print len(B());\n",
    ),
    Case(
        "length_result_type",
        "class L { __len__() { return \"no\"; } }\n"
        "try { len(L()); } catch (e) { print e.kind; }\n",
    ),
    # A __len__ declared with a parameter must raise the ordinary catchable
    # ArityError, not crash the run inside the dispatch.
    Case(
        "length_arity",
        "class L { __len__(x) { return x; } }\n"
        "try { len(L()); } catch (e) { print e.kind; }\n",
    ),
    # print and str() render an Instance through __str__, including nested
    # inside a List or Map; the default stays "ClassName instance".
    Case(
        "str",
        "class S {\n"
        "  init(n) { this.n = n; }\n"
        "  __str__() { return \"S\" + str(this.n); }\n"
        "}\n"
        "class T {}\n"
        "print S(1);\n"
        "print str(S(2));\n"
        "print [S(3), 9];\n"
        "print {\"k\": S(4)};\n"
        "print str(42);\n"
        "print T();\n",
    ),
    Case(
        "str_result_type",
        "class Bad { __str__() { return 42; } }\n"
        "try { var s = str(Bad()); } catch (e) { print e.kind; }\n"
        "try { print Bad(); } catch (e) { print e.kind; }\n",
    ),
    # A __str__ that composes another str() whose __str__ throws a caught
    # error must not corrupt the outer result.
    Case(
        "str_nested_caught",
        "class W { __str__() { return 42; } }\n"
        "class V { __str__() { try { str(W()); } catch (e) {} return \"ok\"; } }\n"
        "print str(V());\n",
    ),
    # Internal equality (list membership) must stay identity, never __eq__.
    Case(
        "identity_membership",
        "class V { __eq__(o) { return true; } }\n"
        "var a = V();\n"
        "var b = V();\n"
        "print a in [b];\n"
        "print a == b;\n",
    ),
    # A dunder declared with the wrong parameter count must raise the ordinary
    # catchable ArityError, not run the method and read args out of bounds.
    Case(
        "dunder_arity",
        "class C { __call__(a, b) { return a + b; } }\n"
        "try { C()(1); } catch (e) { print e.kind; }\n"
        "class V { __add__(a, b) { return a + b; } }\n"
        "try { V() + V(); } catch (e) { print e.kind; }\n"
        "class U { __lt__() { return true; } }\n"
        "try { U() < U(); } catch (e) { print e.kind; }\n"
        "class W { __neg__(x) { return x; } }\n"
        "try { -W(); } catch (e) { print e.kind; }\n"
        "class X { __call__(a) { return a; } }\n"
        "try { X()(1, 2, 3); } catch (e) { print e.kind; }\n",
    ),
    # An Instance Map key: __hash__ places the key and __eq__ resolves a
    # collision. The two must stay consistent, and an update under an equal
    # key must overwrite rather than add.
    Case(
        "map_instance_keys",
        "class K {\n"
        "  init(n) { this.n = n; }\n"
        "  __hash__() { return this.n; }\n"
        "  __eq__(o) { return this.n == o.n; }\n"
        "}\n"
        "var m = {};\n"
        "m[K(1)] = \"one\";\n"
        "m[K(2)] = \"two\";\n"
        "m[K(1)] = \"ONE\";\n"
        "print m[K(1)];\n"
        "print m[K(2)];\n"
        "print len(m);\n"
        "print K(3) in m;\n"
        "print K(1) in m;\n",
    ),
    # An Instance key with only one of __hash__/__eq__ is not a valid key.
    Case(
        "map_instance_key_incomplete",
        "class H { __hash__() { return 1; } }\n"
        "class E { __eq__(o) { return true; } }\n"
        "var m = {};\n"
        "try { m[H()] = 1; } catch (e) { print e.kind; }\n"
        "try { m[E()] = 1; } catch (e) { print e.kind; }\n",
    ),
    # callMethod runs a closure-backed user method and returns its value
    # (the re-entrant call path, issue #496).
    Case(
        "callmethod_closure",
        "class C {\n"
        "  init(x) { this.x = x; }\n"
        "  add(y) { return this.x + y; }\n"
        "}\n"
        "print callMethod(C(10), \"add\", 5);\n",
    ),
]


@dataclass
class RunResult:
    exit_code: int
    stdout: str
    stderr: str
    timed_out: bool = False


def run_case(cmd: list[str], case: Case, workdir: Path, timeout: float,
             env: dict | None = None) -> RunResult:
    program_path = workdir / f"{case.name}.lox"
    program_path.write_text(case.program)
    try:
        proc = subprocess.run(
            [*cmd, str(program_path)],
            capture_output=True,
            text=True,
            timeout=timeout,
            env=env,
        )
    except subprocess.TimeoutExpired:
        return RunResult(exit_code=-1, stdout="", stderr="", timed_out=True)
    return RunResult(exit_code=proc.returncode, stdout=proc.stdout,
                     stderr=proc.stderr)


def check_case(case: Case, native: RunResult, bootstrap: RunResult) -> list[str]:
    problems = []
    if native.timed_out:
        problems.append("native timed out -- corpus bug, fix the case")
        return problems
    if native.exit_code != 0:
        problems.append(
            f"native failed (exit={native.exit_code}, stderr={native.stderr!r}) "
            "-- corpus bug, fix the case")
        return problems
    if bootstrap.timed_out:
        problems.append("bootstrap timed out")
        return problems
    if bootstrap.exit_code != native.exit_code:
        problems.append(
            f"exit code: native={native.exit_code} bootstrap={bootstrap.exit_code}")
    if bootstrap.stdout != native.stdout:
        problems.append(
            f"stdout: native={native.stdout!r} bootstrap={bootstrap.stdout!r}")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--native", default=str(REPO_ROOT / "build" / "loxpp"))
    parser.add_argument("--bootstrap",
                        default=str(REPO_ROOT / "bootstrap" / "lox_wrapper.sh"))
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument(
        "--trace-enabled",
        action="store_true",
        default=os.environ.get("LOXPP_BOOTSTRAP_TRACE_ENABLED") == "1",
        help="skip (ctest SKIP_RETURN_CODE) instead of running: the native binary "
        "under test was built with LOXPP_DEBUG_TRACE_EXECUTION/LOXPP_DEBUG_PRINT_CODE on",
    )
    args = parser.parse_args()

    if args.trace_enabled:
        print("SKIP: native binary was built with debug trace macros on (release preset required)")
        return SKIP_RETURN_CODE

    bootstrap_env = {**os.environ, "LANGUAGE": "LOXPP"}

    failures: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        workdir = Path(tmp)
        for case in CASES:
            native_result = run_case([args.native], case, workdir, args.timeout)
            bootstrap_result = run_case([args.bootstrap], case, workdir,
                                        args.timeout, env=bootstrap_env)
            problems = check_case(case, native_result, bootstrap_result)
            if problems:
                failures.append(f"{case.name}:\n  " + "\n  ".join(problems))

    if failures:
        print(f"{len(failures)}/{len(CASES)} case(s) diverge:\n")
        print("\n\n".join(failures))
        return 1

    print(f"OK: {len(CASES)} operator-overloading case(s) match between native and bootstrap.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
