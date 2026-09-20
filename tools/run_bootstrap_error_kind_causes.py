#!/usr/bin/env python3
"""
Runs one small Lox++ program through the bootstrap interpreter for each
non-table `kind` string tools/check_bootstrap_error_kinds.py's audit gave a
site in bootstrap/loxpp_interpreter.lox, and checks the caught value's
`.kind` matches.

check_bootstrap_error_kinds.py proves the SOURCE TEXT of these calls has not
drifted; it starts no interpreter. bootstrap/loxpp_interpreter.lox is a Lox++
program no compiler checks, so a typo in a branch nothing else exercises
ships silently, and no other test in the suite reaches these branches. This
script is the run-time counterpart the static check cannot be.

Usage: tools/run_bootstrap_error_kind_causes.py
Exits 0 if every cause below reports its expected `kind`; exits 1 and lists
every mismatch otherwise. Exits 125 (ctest's configured SKIP_RETURN_CODE for
this test) without running anything when LOXPP_BOOTSTRAP_TRACE_ENABLED=1 is
set in the environment -- the caller's build/loxpp was built with
LOXPP_DEBUG_TRACE_EXECUTION or LOXPP_DEBUG_PRINT_CODE on (the Debug preset's
default), so its stdout carries a full per-instruction trace of the
bootstrap interpreter's own execution and no single-line `.kind` output
could be read out of it.
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
WRAPPER = REPO_ROOT / "bootstrap" / "lox_wrapper.sh"

# (name, program, expected kind) -- one per non-table `kind` string this
# audit introduced (tools/check_bootstrap_error_kinds.py's
# REVIEWED_NON_TABLE_KINDS), covering every site that uses it.
CASES = [
    (
        "reflectCallMethod field-name type",
        'class Foo {} try { callMethod(Foo(), 42); } catch (e) { print e.kind; }',
        "ReflectionFieldNameError",
    ),
    (
        "getField field-name type",
        'class Foo { init() { this.x = 1; } } '
        'try { getField(Foo(), 42); } catch (e) { print e.kind; }',
        "ReflectionFieldNameError",
    ),
    (
        "hasField field-name type",
        'class Foo { init() { this.x = 1; } } '
        'try { hasField(Foo(), 42); } catch (e) { print e.kind; }',
        "ReflectionFieldNameError",
    ),
    (
        "setField field-name type",
        'class Foo { init() { this.x = 1; } } '
        'try { setField(Foo(), 42, 1); } catch (e) { print e.kind; }',
        "ReflectionFieldNameError",
    ),
    (
        "reflectCallMethod undefined member",
        'class Foo { init() { this.x = 1; } } '
        'try { callMethod(Foo(), "bar"); } catch (e) { print e.kind; }',
        "ReflectionUndefinedMemberError",
    ),
    (
        "reflectCallMethod unsupported user-defined method",
        'class Foo { bar() { return 1; } } '
        'try { callMethod(Foo(), "bar"); } catch (e) { print e.kind; }',
        "ReflectionUnsupportedError",
    ),
    (
        "list.remove value not found",
        'try { [1,2,3].remove(99); } catch (e) { print e.kind; }',
        "ValueNotFoundError",
    ),
    (
        "len() wrong argument type",
        'try { len(42); } catch (e) { print e.kind; }',
        "LengthTypeError",
    ),
    (
        "fields() wrong receiver",
        'try { fields(42); } catch (e) { print e.kind; }',
        "ReflectionReceiverError",
    ),
    (
        "methods() wrong receiver",
        'try { methods(42); } catch (e) { print e.kind; }',
        "ReflectionReceiverError",
    ),
    (
        "getField() wrong receiver",
        'try { getField(42, "x"); } catch (e) { print e.kind; }',
        "ReflectionReceiverError",
    ),
    (
        "hasField() wrong receiver",
        'try { hasField(42, "x"); } catch (e) { print e.kind; }',
        "ReflectionReceiverError",
    ),
    (
        "setField() wrong receiver",
        'try { setField(42, "x", 1); } catch (e) { print e.kind; }',
        "ReflectionReceiverError",
    ),
    (
        "callMethod() own arity",
        'try { callMethod(); } catch (e) { print e.kind; }',
        "ReflectionArityError",
    ),
    (
        "string index assignment",
        'try { "abc"[0] = "x"; } catch (e) { print e.kind; }',
        "StringImmutableError",
    ),
    (
        "slice of a non-List/String",
        'try { (42)[0:1]; } catch (e) { print e.kind; }',
        "NotSliceableError",
    ),
    (
        "slice bound wrong type",
        'try { [1, 2]["a":2]; } catch (e) { print e.kind; }',
        "SliceIndexTypeError",
    ),
    (
        "slice bound not integer",
        'try { [1, 2][1.5:2]; } catch (e) { print e.kind; }',
        "SliceIndexNotIntegerError",
    ),
    (
        "slice bound negative",
        'try { [1, 2][-1:2]; } catch (e) { print e.kind; }',
        "SliceIndexNegativeError",
    ),
    (
        "'in' left operand wrong type",
        'try { 1 in "abc"; } catch (e) { print e.kind; }',
        "InLeftOperandTypeError",
    ),
    (
        "'in' right operand wrong type",
        "try { 1 in 42; } catch (e) { print e.kind; }",
        "InRightOperandTypeError",
    ),
    (
        "undefined member on an Instance",
        'class C {} try { C().bogus; } catch (e) { print e.kind; }',
        "UndefinedMemberError",
    ),
    (
        "undefined member on a List",
        "try { [].bogus; } catch (e) { print e.kind; }",
        "UndefinedMemberError",
    ),
    (
        "undefined member on a Map",
        "try { var m = {}; m.bogus; } catch (e) { print e.kind; }",
        "UndefinedMemberError",
    ),
    (
        "undefined member via super",
        "class A {} class B < A { m() { super.zzz(); } } "
        "try { B().m(); } catch (e) { print e.kind; }",
        "UndefinedMemberError",
    ),
    (
        "property write on a non-instance",
        "try { 42.foo = 1; } catch (e) { print e.kind; }",
        "InvalidFieldReceiverError",
    ),
]


def run_case(program: str) -> str:
    with tempfile.NamedTemporaryFile("w", suffix=".lox", delete=False) as f:
        # A source file with no trailing newline makes the scanner mis-lex
        # the final token at EOF (a pre-existing scanner quirk, not
        # something this probe should work around silently).
        f.write(program + "\n")
        path = f.name
    try:
        env = dict(os.environ, LANGUAGE="LOXPP")
        result = subprocess.run(
            [str(WRAPPER), path],
            capture_output=True,
            text=True,
            env=env,
            timeout=30,
            check=False,
        )
        return result.stdout.strip(), result.returncode
    finally:
        os.unlink(path)


def main() -> int:
    if os.environ.get("LOXPP_BOOTSTRAP_TRACE_ENABLED") == "1":
        print(
            "SKIP: build/loxpp was built with LOXPP_DEBUG_TRACE_EXECUTION or "
            "LOXPP_DEBUG_PRINT_CODE on, so its stdout is a per-instruction trace, "
            "not the plain `.kind` line this test reads. Build with the release "
            "preset (both flags off) to run this test for real.",
            file=sys.stderr,
        )
        return 125

    failures = []
    for name, program, expected in CASES:
        actual, exit_code = run_case(program)
        if exit_code != 0:
            failures.append(f"{name}: exited {exit_code} (expected 0), stdout {actual!r}")
        elif actual != expected:
            failures.append(f"{name}: expected kind {expected!r}, got {actual!r}")
    if failures:
        print(f"{len(failures)} bootstrap error-kind runtime mismatch(es):\n", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1
    print(f"OK: {len(CASES)} bootstrap error-kind causes checked at run time, all match.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
