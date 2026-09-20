#!/usr/bin/env python3
"""
Differential runtime check: for every distinct (kind, message) pair
tools/check_bootstrap_error_kinds.py's EXPECTED_CALLS baseline records, runs
one or more small Lox++ programs through BOTH the native VM (`build/loxpp`)
and the bootstrap interpreter (`bootstrap/lox_wrapper.sh`), and checks that
each program's outcome on native is the one this script declares:

- "catchable": native must catch the fault, print the expected `kind`, and
  exit 0 -- proof this cause really is a row of spec/04-semantics.md's
  catchable Runtime Errors table on native, not only on bootstrap.
- "fatal": native must exit 70 and never print a `.kind` line -- proof this
  cause is NOT a native table row, so bootstrap giving it a non-table `kind`
  is not an undetected alias.

Every probe also runs on bootstrap and must catch the fault with the exact
`kind` this script declares, whichever disposition it has -- bootstrap
converts every one of these causes to a catchable Error value by design;
what differs by disposition is only what native does with the same program.

A manifest with a human judgment column ("JUDGED") lets a person re-read the
same site list with a slightly different lens each time and still miss a
site the same shape as one already fixed. This script turns the checkpoint's
claim -- "this bootstrap kind is a genuine alias of a native table row" /
"this bootstrap kind is genuinely distinct" -- into an executable fact about
native's own behavior, not a claim about a column a person filled in.

Two rules make the whole EXPECTED_CALLS list provably non-aliasing, by
construction, without anyone re-reading it:

1. Coverage rule: every distinct (kind, message) pair in EXPECTED_CALLS
   (imported from check_bootstrap_error_kinds -- never copied here) must
   have at least one probe. A DYNAMIC pair (its kind is chosen at run time)
   counts as covered only when every kind it can resolve to
   (DYNAMIC_RESOLUTIONS below) has its own probe.
2. Alias rule: a pair whose kind is a spec/04-semantics.md table kind must
   have at least one "catchable" probe -- proof native really does deliver
   that cause catchably under that kind. A pair whose kind is NOT a table
   kind must have ONLY "fatal" probes -- proof native never delivers that
   cause catchably at all, so bootstrap's own kind cannot be aliasing a
   native table row nobody has seen.

One exemption: `MaxDepthExceededError` / "Value nesting is too deep." is
fatal on native yet keeps its table kind, because spec/04-semantics.md
names this exact fault in both the catchable and the Fatal Runtime Errors
tables, and issue #338, not this script, decides which disposition is
correct.

Three pairs are "fused": the same bootstrap call site serves two different
native causes, one catchable and one fatal, because bootstrap's tree-walker
cannot tell them apart the way native's opcode-level dispatch can (the same
shape as `evalGet`'s `InvalidReceiverError` ambiguity between a property get
and a method call). Each of these three pairs carries BOTH a catchable and
a fatal probe deliberately -- that is not an alias, it is the exact
catchability divergence being pinned so a future differential node (#336)
can see it.

Usage: tools/run_bootstrap_error_kind_causes.py
Exits 0 if every rule holds and every probe's native and bootstrap outcome
matches what it declares; exits 1 and lists every failure otherwise. Exits
125 (ctest's configured SKIP_RETURN_CODE for this test) without running
anything when LOXPP_BOOTSTRAP_TRACE_ENABLED=1 is set in the environment --
the caller's build/loxpp was built with LOXPP_DEBUG_TRACE_EXECUTION or
LOXPP_DEBUG_PRINT_CODE on (the Debug preset's default), so its stdout
carries a full per-instruction trace of the bootstrap interpreter's own
execution and no single-line `.kind` output could be read out of it.
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
NATIVE = REPO_ROOT / "build" / "loxpp"
WRAPPER = REPO_ROOT / "bootstrap" / "lox_wrapper.sh"

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_bootstrap_error_kinds import (  # noqa: E402
    DYNAMIC,
    EXPECTED_CALLS,
    SPEC_PATH,
    load_spec_table_kinds,
)

CATCHABLE = "catchable"
FATAL = "fatal"

# A DYNAMIC EXPECTED_CALLS entry's real kind is chosen at run time; this maps
# each DYNAMIC message to every kind it can resolve to (see EXPECTED_CALLS's
# own comment in check_bootstrap_error_kinds.py). Coverage for a DYNAMIC pair
# means every kind listed here has its own probe, not the placeholder itself.
DYNAMIC_RESOLUTIONS = {
    '"Map key must be a scalar (Nil, Bool, Number, or String)."': (
        "InvalidMapKeyError",
        "NaNKeyError",
    ),
    '"Expected " + str(callee.arity()) + " arguments but got " + str(len(args)) + "."': (
        "ArityError",
        "ConstructorArityError",
    ),
}

# The three fused pairs (see module docstring): each keeps a table kind but
# is deliberately allowed both a catchable AND a fatal probe.
FUSED_PAIRS = {
    ("NotCallableError", '"Can only call functions and classes."'),
    ("InvalidReceiverError", '"Only instances have properties."'),
    ("ArityError", '"Expected " + str(callee.arity()) + " arguments but got " + str(len(args)) + "."'),
}

# The one exemption (see module docstring): table kind, fatal-only, pending #338.
MAXDEPTH_EXEMPT = {("MaxDepthExceededError", '"Value nesting is too deep."')}

# (name, program, disposition, kind, message) -- `message` must equal, byte
# for byte, an EXPECTED_CALLS message string (or a DYNAMIC message's
# resolved-kind pairing) so the coverage/alias checks below can match a
# probe back to the baseline entry it proves.
PROBES = [
    # --- non-table kinds: every probe here must be "fatal" (alias rule) ---
    ("callMethod() own arity", 'try { callMethod(); } catch (e) { print e.kind; }',
     FATAL, "ReflectionArityError", '"Expected at least 2 arguments."'),
    ("callMethod() field-name type", 'class Foo {} try { callMethod(Foo(), 42); } catch (e) { print e.kind; }',
     FATAL, "ReflectionFieldNameError", '"Field name must be a string."'),
    ("callMethod() undefined member",
     'class Foo { init() { this.x = 1; } } try { callMethod(Foo(), "bar"); } catch (e) { print e.kind; }',
     FATAL, "ReflectionUndefinedMemberError", '"Undefined property \'" + name + "\'."'),
    ("callMethod() unsupported user-defined method",
     'class Foo { bar() { return 1; } } try { callMethod(Foo(), "bar"); } catch (e) { print e.kind; }',
     FATAL, "ReflectionUnsupportedError", '"callMethod does not support user-defined methods yet."'),
    ("len() wrong argument type", 'try { len(42); } catch (e) { print e.kind; }',
     FATAL, "LengthTypeError", '"len() argument must be a String, List, or Map."'),
    ("fields() wrong receiver", 'try { fields(42); } catch (e) { print e.kind; }',
     FATAL, "ReflectionReceiverError", '"Expected an instance."'),
    ("methods() wrong receiver", 'try { methods(42); } catch (e) { print e.kind; }',
     FATAL, "ReflectionReceiverError", '"Expected a class."'),
    ("getField() wrong receiver", 'try { getField(42, "x"); } catch (e) { print e.kind; }',
     FATAL, "ReflectionReceiverError", '"Only instances have properties."'),
    ("setField() wrong receiver", 'try { setField(42, "x", 1); } catch (e) { print e.kind; }',
     FATAL, "ReflectionReceiverError", '"Only instances have fields."'),
    ("callMethod() wrong receiver", 'try { callMethod(42, "foo"); } catch (e) { print e.kind; }',
     FATAL, "ReflectionReceiverError", '"Only instances have methods."'),
    ("callMethod() not-callable field",
     'class C { init() { this.x = 1; } } try { callMethod(C(), "x"); } catch (e) { print e.kind; }',
     FATAL, "ReflectionNotCallableError", '"callMethod requires a callable value."'),
    ("callMethod() forwarded native arity mismatch",
     'class C { init() { this.f = len; } } try { callMethod(C(), "f"); } catch (e) { print e.kind; }',
     FATAL, "ReflectionNativeArityError",
     '"Expected " + str(callee.arity()) + " arguments but got " +\n                         str(len(forwarded)) + "."'),
    ("list.remove value not found", 'try { [1,2,3].remove(99); } catch (e) { print e.kind; }',
     FATAL, "ValueNotFoundError", '"Value not found in list."'),
    ("string index assignment", 'try { "abc"[0] = "x"; } catch (e) { print e.kind; }',
     FATAL, "StringImmutableError", '"Strings are immutable."'),
    ("slice of a non-List/String", 'try { (42)[0:1]; } catch (e) { print e.kind; }',
     FATAL, "NotSliceableError", '"Slice requires a List or String."'),
    ("slice bound wrong type", 'try { [1, 2]["a":2]; } catch (e) { print e.kind; }',
     FATAL, "SliceIndexTypeError", '"Slice index must be a number."'),
    ("slice bound not integer", 'try { [1, 2][1.5:2]; } catch (e) { print e.kind; }',
     FATAL, "SliceIndexNotIntegerError", '"Slice index must be an integer."'),
    ("slice bound negative", 'try { [1, 2][-1:2]; } catch (e) { print e.kind; }',
     FATAL, "SliceIndexNegativeError", '"Slice index must be non-negative."'),
    ("'in' left operand wrong type", 'try { 1 in "abc"; } catch (e) { print e.kind; }',
     FATAL, "InLeftOperandTypeError", '"Left operand of \'in\' on a String must be a String."'),
    ("'in' right operand wrong type", "try { 1 in 42; } catch (e) { print e.kind; }",
     FATAL, "InRightOperandTypeError", '"Right operand of \'in\' must be a List, String, or Map."'),
    ("undefined member on an Instance", 'class C {} try { C().bogus; } catch (e) { print e.kind; }',
     FATAL, "UndefinedMemberError", '"Undefined property \'" + name + "\'."'),
    ("undefined member via super",
     "class A {} class B < A { m() { super.zzz(); } } try { B().m(); } catch (e) { print e.kind; }",
     FATAL, "UndefinedMemberError", '"Undefined property \'" + method + "\'."'),
    ("property write on a non-instance", "try { 42.foo = 1; } catch (e) { print e.kind; }",
     FATAL, "InvalidFieldReceiverError", '"Only instances have fields."'),
    ("for-in on a non-iterable", "try { for (var x in 42) {} } catch (e) { print e.kind; }",
     FATAL, "ForInNotIterableError", '"Value is not iterable (expected list, string, or map)."'),
    ("map size changed during for-in", "try { var m = {1: 1}; for (var k in m) { m[2] = 2; } } catch (e) { print e.kind; }",
     FATAL, "MapSizeChangedError", '"Map changed size during iteration."'),
    ("superclass is not a class",
     "var N = 1; fun f() { class Sub < N {} } try { f(); } catch (e) { print e.kind; }",
     FATAL, "InvalidSuperclassError", '"Superclass must be a class."'),
    ("object destructuring of a non-instance", "try { var {a} = 42; } catch (e) { print e.kind; }",
     FATAL, "InvalidDestructureReceiverError", '"Object destructuring requires an instance."'),

    # --- table kinds: every distinct pair needs >=1 "catchable" probe ---
    ("stack overflow", 'fun rec(n) { return rec(n + 1); } try { rec(0); } catch (e) { print e.kind; }',
     CATCHABLE, "StackOverflowError", '"Stack overflow."'),
    ("pop on empty list", "try { [].pop(); } catch (e) { print e.kind; }",
     CATCHABLE, "EmptyListError", '"Cannot pop from an empty list."'),
    ("list index wrong type", 'try { var l = [1]; l["a"]; } catch (e) { print e.kind; }',
     CATCHABLE, "IndexTypeError", '"Index must be a number."'),
    ("list index not an integer", "try { var l = [1]; l[1.5]; } catch (e) { print e.kind; }",
     CATCHABLE, "IndexNotIntegerError", '"Index must be an integer."'),
    ("list index out of bounds", "try { [][0]; } catch (e) { print e.kind; }",
     CATCHABLE, "IndexOutOfBoundsError", '"List index out of bounds."'),
    ("index-get of a non-indexable value", "try { 42[0]; } catch (e) { print e.kind; }",
     CATCHABLE, "NotIndexableError", '"Only lists, maps, and strings can be indexed."'),
    ("index-set of a non-indexable value", "try { 42[0] = 1; } catch (e) { print e.kind; }",
     CATCHABLE, "NotIndexableError", '"Only lists and maps support index assignment."'),
    ("sequence destructuring of a non-List", "try { var [a] = 42; } catch (e) { print e.kind; }",
     CATCHABLE, "NotIndexableError", '"Sequence destructuring requires a List."'),
    ("sequence destructuring too few elements", "try { var [a, b] = [1]; } catch (e) { print e.kind; }",
     CATCHABLE, "IndexOutOfBoundsError", '"Not enough elements for sequence destructuring."'),
    ("NaN used as map key", "try { var m = {}; m[0/0] = 1; } catch (e) { print e.kind; }",
     CATCHABLE, "NaNKeyError", '"Map key must be a scalar (Nil, Bool, Number, or String)."'),
    ("object used as map key", "try { var m = {}; m[[1, 2]] = 1; } catch (e) { print e.kind; }",
     CATCHABLE, "InvalidMapKeyError", '"Map key must be a scalar (Nil, Bool, Number, or String)."'),
    ("undefined global variable", "try { print undeclared; } catch (e) { print e.kind; }",
     CATCHABLE, "UndefinedVariableError", '"Undefined variable \'" + name + "\'."'),
    ("unary arithmetic on non-Number", 'try { -"a"; } catch (e) { print e.kind; }',
     CATCHABLE, "ArithmeticTypeError", '"Operand must be a number."'),
    ("binary arithmetic on non-Numbers", 'try { "a" - 1; } catch (e) { print e.kind; }',
     CATCHABLE, "ArithmeticTypeError", '"Operands must be numbers."'),
    ("'+' on incompatible types", 'try { 1 + "a"; } catch (e) { print e.kind; }',
     CATCHABLE, "ConcatenationTypeError", '"Operands must be two numbers or two strings."'),
    ("comparison on non-Numbers", 'try { "a" < 1; } catch (e) { print e.kind; }',
     CATCHABLE, "ComparisonTypeError", '"Operands must be numbers."'),
    ("undefined property on an Error value",
     "try { try { [][0]; } catch (e) { e.foo; } } catch (e2) { print e2.kind; }",
     CATCHABLE, "UndefinedPropertyError", '"Undefined property \'" + name + "\' on error."'),
    ("enum constructor called with wrong arity",
     "enum E { A(x) } try { A(1, 2); } catch (e) { print e.kind; }",
     CATCHABLE, "ConstructorArityError",
     '"Expected " + str(callee.arity()) + " arguments but got " + str(len(args)) + "."'),
    ("match with no matching arm", 'try { match 99 { case 1 => "one" }; } catch (e) { print e.kind; }',
     CATCHABLE, "MatchError", '"Match expression was not exhaustive."'),

    # --- fused pairs: same bootstrap kind, one catchable probe and one fatal probe ---
    ("call of a non-callable value (fused, catchable)", "try { 42(); } catch (e) { print e.kind; }",
     CATCHABLE, "NotCallableError", '"Can only call functions and classes."'),
    ("field shadowing a method, not callable (fused, fatal)",
     "class C { init() { this.f = 1; } } try { C().f(); } catch (e) { print e.kind; }",
     FATAL, "NotCallableError", '"Can only call functions and classes."'),
    ("wrong argument count (fused, catchable)", "fun f(a) {} try { f(1, 2); } catch (e) { print e.kind; }",
     CATCHABLE, "ArityError",
     '"Expected " + str(callee.arity()) + " arguments but got " + str(len(args)) + "."'),
    ("native function called with wrong arity (fused, fatal)", "try { len(); } catch (e) { print e.kind; }",
     FATAL, "ArityError",
     '"Expected " + str(callee.arity()) + " arguments but got " + str(len(args)) + "."'),
    ("method call on a non-instance (fused, catchable)", "try { 42.foo(); } catch (e) { print e.kind; }",
     CATCHABLE, "InvalidReceiverError", '"Only instances have properties."'),
    ("plain property get on a non-instance (fused, fatal)", "try { 42.foo; } catch (e) { print e.kind; }",
     FATAL, "InvalidReceiverError", '"Only instances have properties."'),

    # --- the one exemption: table kind, fatal-only, pending #338 ---
    ("value nested too deep to print",
     "var a = [1]; var i = 0; while (i < 300) { a = [a]; i = i + 1; } try { print a; } catch (e) { print e.kind; }",
     FATAL, "MaxDepthExceededError", '"Value nesting is too deep."'),
]


def run_case(program: str, interpreter_argv: list[str]) -> tuple[str, int]:
    with tempfile.NamedTemporaryFile("w", suffix=".lox", delete=False) as f:
        # A source file with no trailing newline makes the scanner mis-lex
        # the final token at EOF (a pre-existing scanner quirk, not
        # something this probe should work around silently).
        f.write(program + "\n")
        path = f.name
    try:
        env = dict(os.environ, LANGUAGE="LOXPP")
        result = subprocess.run(
            [*interpreter_argv, path],
            capture_output=True,
            text=True,
            env=env,
            timeout=30,
            check=False,
        )
        return result.stdout.strip(), result.returncode
    finally:
        os.unlink(path)


def required_targets(kind: str, message: str) -> tuple[str, ...]:
    """Every kind this (kind, message) EXPECTED_CALLS pair must have a probe
    for. A literal kind needs only itself; a DYNAMIC pair needs every kind
    DYNAMIC_RESOLUTIONS says it can resolve to."""
    if kind == DYNAMIC:
        resolved = DYNAMIC_RESOLUTIONS.get(message)
        if resolved is None:
            raise SystemExit(
                f"run_bootstrap_error_kind_causes.py: no DYNAMIC_RESOLUTIONS entry for "
                f"message {message!r}. A new DYNAMIC .setError( site was added to "
                "EXPECTED_CALLS without teaching this script what kinds it can resolve "
                "to -- add it to DYNAMIC_RESOLUTIONS before this check can trust its own "
                "coverage claim."
            )
        return resolved
    return (kind,)


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

    spec_kinds = load_spec_table_kinds(SPEC_PATH.read_text(encoding="utf-8"))
    failures: list[str] = []

    # --- static rules: coverage + alias, before running a single program ---

    required_pairs: set[tuple[str, str]] = set()
    for kind, message, _status in EXPECTED_CALLS:
        for target in required_targets(kind, message):
            required_pairs.add((target, message))

    probes_by_pair: dict[tuple[str, str], list[tuple[str, str, str]]] = {}
    for name, _program, disposition, kind, message in PROBES:
        probes_by_pair.setdefault((kind, message), []).append((disposition, name, kind))

    for pair in sorted(required_pairs):
        target_kind, message = pair
        matches = probes_by_pair.get(pair, [])
        if not matches:
            failures.append(
                f"coverage: no probe covers ({target_kind!r}, {message[:50]!r}). "
                "Add one to PROBES in tools/run_bootstrap_error_kind_causes.py."
            )
            continue
        dispositions = {d for d, _n, _k in matches}
        is_table = target_kind in spec_kinds
        if is_table:
            if CATCHABLE not in dispositions and pair not in MAXDEPTH_EXEMPT:
                failures.append(
                    f"alias: {target_kind!r} is a spec/04-semantics.md table kind, but "
                    f"({target_kind!r}, {message[:50]!r}) has no 'catchable' probe -- "
                    "nothing proves native actually delivers this cause catchably "
                    "under this kind."
                )
            if FATAL in dispositions and pair not in FUSED_PAIRS and pair not in MAXDEPTH_EXEMPT:
                failures.append(
                    f"alias: ({target_kind!r}, {message[:50]!r}) is a table kind with a "
                    "'fatal' probe, but it is not one of the FUSED_PAIRS entries and not "
                    "the MAXDEPTH_EXEMPT entry. No other pair may mix a fatal probe with "
                    "a table kind."
                )
        else:
            if CATCHABLE in dispositions:
                failures.append(
                    f"alias: {target_kind!r} is not a spec/04-semantics.md table kind, but "
                    f"({target_kind!r}, {message[:50]!r}) has a 'catchable' probe -- if "
                    "native really delivers this cause catchably, it belongs in the "
                    "spec table, not in REVIEWED_NON_TABLE_KINDS."
                )
            if dispositions - {FATAL}:
                failures.append(
                    f"({target_kind!r}, {message[:50]!r}) has a probe with an unexpected "
                    f"disposition {dispositions - {FATAL}!r}; non-table pairs may only "
                    "have 'fatal' probes."
                )

    if failures:
        print(f"{len(failures)} bootstrap error-kind static rule failure(s):\n", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1

    # --- run every probe for real, on both interpreters ---

    for name, program, disposition, kind, _message in PROBES:
        native_out, native_exit = run_case(program, [str(NATIVE)])
        boot_out, boot_exit = run_case(program, [str(WRAPPER)])

        # For fused pairs (Wave 3, issue #348), bootstrap should now match native:
        # fatal on both, or catchable on both. Check the pair against the fused list.
        is_fused = (kind, _message) in FUSED_PAIRS

        if is_fused and disposition == FATAL:
            # Bootstrap should now be fatal, matching native
            if boot_exit != 70:
                failures.append(
                    f"{name}: declared 'fatal' (fused, Wave 3 fix), but bootstrap exited "
                    f"{boot_exit}, expected 70"
                )
        else:
            # All other cases: bootstrap should catch the error
            if boot_exit != 0 or boot_out != kind:
                failures.append(
                    f"{name}: bootstrap expected to catch {kind!r} and exit 0, got "
                    f"exit {boot_exit}, stdout {boot_out!r}"
                )

        if disposition == CATCHABLE:
            if native_exit != 0 or native_out != kind:
                failures.append(
                    f"{name}: declared 'catchable' with kind {kind!r}, but native gave "
                    f"exit {native_exit}, stdout {native_out!r}"
                )
        else:
            if native_exit == 0:
                failures.append(
                    f"{name}: declared 'fatal', but native exited 0 (stdout {native_out!r}) "
                    "-- this cause is catchable on native after all, so this kind may be "
                    "aliasing an undocumented native table row."
                )

    if failures:
        print(f"{len(failures)} bootstrap error-kind runtime mismatch(es):\n", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1

    print(
        f"OK: {len(required_pairs)} bootstrap error-kind pairs covered by "
        f"{len(PROBES)} probes, 0 uncovered, 0 alias-rule failures, all runtime checks match."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
