#!/usr/bin/env python3
"""
Mission #288 node #336: differential fault test across all four consumers.

For every row of spec/04-semantics.md's Runtime Errors table (the catchable
table) and its Fatal Runtime Errors table, plus the five Error-as-receiver
reflection rows spec/03-types.md's Error section implies (fields/getField/
hasField/setField/callMethod all reject an Error value), this script runs
one small Lox++ program through all four consumers -- native (build/loxpp),
JVM (tools/loxpp_jvm.sh), CLR (tools/loxpp_clr.sh), and the bootstrap
interpreter (bootstrap/lox_wrapper.sh) -- and checks that they agree on:

  - whether the fault is CAUGHT (delivered to a catchBlock) or FATAL
    (halts the program, never reaching the catchBlock);
  - for a CAUGHT row: `e.kind`, `e.message`, `type(e)`, and `str(e)`.

A row's own disposition (catchable vs. fatal) is native's -- native defines
the ground truth (spec/04-semantics.md). Every row is therefore run with
the SAME program shape on all four consumers and classified by what its
stdout actually contains, not by which table the row came from: a
consumer that disagrees with native's disposition shows up as an outcome
mismatch, exactly like a kind or message mismatch would.

Two known outcomes are not run through this comparison at all: a row's
`skip` set. A row is skipped for a consumer only when an earlier mission
node's review already found and filed the exact divergence (see each row's
own comment). Skipping a row does not weaken the check for the other three
consumers running that same row.

Usage:
    tools/check_fault_table.py [--native PATH] [--jvm PATH] [--clr PATH]
                                [--bootstrap PATH] [--timeout SECONDS]

Exits 0 when every non-skipped (row, consumer) pair matches native's
outcome on caught-vs-halted, and (for a caught row) on kind/message/type/
str wherever that field is not itself skipped for that consumer. Exits 1
and prints every mismatch otherwise.
"""

import argparse
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SPEC_PATH = REPO_ROOT / "spec" / "04-semantics.md"

NATIVE = "native"
JVM = "jvm"
CLR = "clr"
BOOTSTRAP = "bootstrap"

# A row's program prints these markers (never "kind"/"message"/"type"/"str"
# text a fault message could itself contain) so a run's outcome is read from
# a fixed vocabulary, not guessed from arbitrary program output.
CAUGHT_MARKER = "__CAUGHT__"
AFTER_MARKER = "__AFTER__"
UNREACHABLE_MARKER = "__UNREACHABLE__"

CAUGHT_TEMPLATE = """{setup}try {{
    {body}
}} catch (e) {{
    print "{caught}";
    print e.kind;
    print e.message;
    print type(e);
    print str(e);
}}
print "{after}";
"""

FATAL_TEMPLATE = """{setup}try {{
    {body}
    print "{unreachable}";
}} catch (_) {{
    print "{caught}";
}}
print "{after}";
"""


@dataclass
class Row:
    name: str  # short id, used in output and as the probe file stem
    disposition: str  # "caught" or "fatal" -- native's own ground truth
    body: str  # the fault-causing code, ending in ';'
    setup: str = ""  # code that runs before the try, ending in ';' (or "")
    expected_kind: str | None = None  # required when disposition == "caught"
    expected_message: str | None = None  # spec's Message column literal, when disposition == "fatal"
    skip: dict[str, str] = field(default_factory=dict)  # consumer -> reason, skips the whole row
    skip_fields: dict[str, set[str]] = field(default_factory=dict)  # consumer -> {"type", "str", "message"}

    def program(self, workdir: Path | None = None) -> str:
        if self.disposition == "caught":
            text = CAUGHT_TEMPLATE.format(
                setup=self.setup, body=self.body, caught=CAUGHT_MARKER, after=AFTER_MARKER
            )
        else:
            text = FATAL_TEMPLATE.format(
                setup=self.setup,
                body=self.body,
                unreachable=UNREACHABLE_MARKER,
                caught=CAUGHT_MARKER,
                after=AFTER_MARKER,
            )
        # A row's body/setup may carry the literal token __WORKDIR__ (never a
        # `.format()` placeholder, since body/setup themselves can hold `{`
        # and `}` from map/set literals) in place of a file path, so a File
        # row writes only inside this run's own temporary directory.
        if workdir is not None:
            text = text.replace("__WORKDIR__", str(workdir))
        return text


# --- Catchable Runtime Errors table (spec/04-semantics.md, 19 rows) -------
#
# Most Example cells are bare expressions the spec table lists standalone;
# a few reference a name the cell itself never declares (`m`, `list`) or are
# prose, not code ("Unbounded recursion") -- `setup` below supplies exactly
# what each such cell is missing, without changing the fault it exercises.
CATCHABLE_ROWS = [
    Row("arithmetic_type_error", "caught", '"a" - 1;', expected_kind="ArithmeticTypeError"),
    Row("comparison_type_error", "caught", '"a" < 1;', expected_kind="ComparisonTypeError"),
    Row("concatenation_type_error", "caught", '1 + "a";', expected_kind="ConcatenationTypeError"),
    Row("not_callable_error", "caught", "42();", expected_kind="NotCallableError"),
    Row("arity_error", "caught", "fun f(a) {} f(1, 2);", expected_kind="ArityError"),
    Row("undefined_variable_error", "caught", "print undeclared;", expected_kind="UndefinedVariableError"),
    # `f` is declared OUTSIDE the try (setup), not inside it: a self-recursive
    # local function declared inside a try block fails JVM bytecode
    # verification (issue #350) -- an emission bug unrelated to this table
    # row, which a top-level declaration avoids while still exercising the
    # same StackOverflowError fault.
    Row(
        "stack_overflow_error",
        "caught",
        "f();",
        setup="fun f() { return f(); }\n",
        expected_kind="StackOverflowError",
    ),
    Row("not_indexable_error", "caught", "42[0];", expected_kind="NotIndexableError"),
    Row("index_type_error", "caught", 'list["a"];', setup="var list = [];\n", expected_kind="IndexTypeError"),
    Row(
        "index_not_integer_error",
        "caught",
        "list[1.5];",
        setup="var list = [];\n",
        expected_kind="IndexNotIntegerError",
    ),
    Row("index_out_of_bounds_error", "caught", "[][0];", expected_kind="IndexOutOfBoundsError"),
    Row("empty_list_error", "caught", "[].pop();", expected_kind="EmptyListError"),
    Row("nan_key_error", "caught", "m[0/0] = 1;", setup="var m = {};\n", expected_kind="NaNKeyError"),
    Row(
        "invalid_map_key_error",
        "caught",
        "m[[1,2]] = 1;",
        setup="var m = {};\n",
        expected_kind="InvalidMapKeyError",
    ),
    # MaxDepthExceededError: native reports this fatal today (see the
    # FATAL_ROWS entry below), the opposite disposition from this table
    # row. Issue #338 owns which disposition is correct; this test does not
    # run this row, on any consumer, until that is settled.
    Row("invalid_receiver_error", "caught", "42.foo();", expected_kind="InvalidReceiverError"),
    # Regression row for issue #348: calling a plain non-callable value must
    # stay catchable even after a previous field read. The old code tracked
    # field reads with interpreter state that could leak across statements,
    # making `42()` fatal when a field was read earlier.
    Row(
        "call_non_callable_after_field_read",
        "caught",
        "42();",
        setup="class C { init() { this.g = 1; } } var c = C(); var y = c.g;\n",
        expected_kind="NotCallableError",
    ),
    Row(
        "match_error",
        "caught",
        'match 99 { case 1 => "one" };',
        expected_kind="MatchError",
    ),
    Row(
        "constructor_arity_error",
        "caught",
        "ok(1, 2);",
        setup="enum E { ok(x) }\n",
        expected_kind="ConstructorArityError",
    ),
    # This row's own Example is already a nested try/catch (the fault it
    # names -- an undefined property read on a caught Error -- can only be
    # produced by first catching one). The outer catch here is the probe's
    # own instrumentation, not part of the row's Example.
    Row(
        "undefined_property_on_error",
        "caught",
        "try { [][0]; } catch (e) { e.foo; }",
        expected_kind="UndefinedPropertyError",
    ),
]

# --- Fatal Runtime Errors table (spec/04-semantics.md) --------------------
#
# Every Example below is self-contained and runnable as its own program.
FATAL_ROWS = [
    # `enum` is only legal at global scope, so the declaration goes in
    # `setup` (emitted before the `try`); only the faulting `match` goes in
    # `body`.
    Row(
        "get_tag_non_enum",
        "fatal",
        "match 1 { case Ok(v) => v case Err(m) => -1 };",
        setup="enum Result { Ok(v) Err(m) }\n",
        expected_message="GET_TAG: expected an enum value.",
    ),
    # Value nested too deep to print: fatal on native today, with no
    # `Error.kind`, but this exact fault keeps a catchable row
    # (MaxDepthExceededError) in the table above -- the opposite
    # disposition. Issue #338 decides which is correct; until then, this is
    # not one settled ground truth to diff the other three consumers
    # against, so this test does not run it.
    Row(
        "stdlib_native_arity",
        "fatal",
        "clock(1);",
        expected_message="Expected 0 arguments but got 1.",
    ),
    # A stdlib native's own error text is not enumerated by spec/04-semantics.md
    # (see the Fatal Runtime Errors section's own note); native and the JVM
    # backend report different text for the same open() failure (native names
    # the path once, the JVM's IOException message repeats it). Disposition
    # (fatal on every consumer) is still checked; the message text is not.
    Row(
        "stdlib_open_failure",
        "fatal",
        'open("/no/such/path", "r");',
        skip_fields={JVM: {"message"}, CLR: {"message"}, BOOTSTRAP: {"message"}},
        expected_message="open(): cannot open '/no/such/path': No such file or directory",
    ),
    Row(
        "undefined_property_on_file",
        "fatal",
        'var f = open("__WORKDIR__/loxpp_fault_table_probe.txt", "w"); f.write("x"); '
        'var g = open("__WORKDIR__/loxpp_fault_table_probe.txt", "r"); g.bogus;',
        expected_message="Undefined property 'bogus' on file.",
    ),
    Row(
        "undefined_property_on_map",
        "fatal",
        "var m = {}; m.bogus;",
        expected_message="Undefined property 'bogus' on map.",
    ),
    Row(
        "undefined_property_on_instance",
        "fatal",
        "class C {} var c = C(); c.bogus;",
        expected_message="Undefined property 'bogus'.",
    ),
    # Fused site (issue #348): bootstrap's tree-walker used to give this
    # the same catchable kind it gives `42.foo()` (a call), because it could
    # not tell a plain property-get apart from a call target. Now it
    # distinguishes them by tracking whether property access is a call target.
    Row(
        "property_get_non_instance",
        "fatal",
        "42.foo;",
        expected_message="Only instances have properties.",
    ),
    Row(
        "property_set_non_instance",
        "fatal",
        "42.foo = 1;",
        expected_message="Only instances have fields.",
    ),
    # Fused site (issue #348): bootstrap now distinguishes property-access
    # sources, making calls to non-callable field values fatal while keeping
    # calls to non-callable literal values catchable.
    Row(
        "invoke_field_not_callable",
        "fatal",
        "class C { init() { this.f = 1; } } C().f();",
        expected_message="Can only call functions, classes and enums.",
    ),
    Row(
        "invoke_method_not_found",
        "fatal",
        "class C {} C().bogus();",
        expected_message="Undefined property 'bogus'.",
    ),
    Row(
        "list_append_wrong_arity",
        "fatal",
        "[].append();",
        expected_message="'append' expects 1 argument but got 0.",
    ),
    Row(
        "list_pop_wrong_arity",
        "fatal",
        "[].pop(1);",
        expected_message="'pop' expects 0 arguments but got 1.",
    ),
    Row(
        "list_remove_wrong_arity",
        "fatal",
        "[1].remove();",
        expected_message="'remove' expects 1 argument but got 0.",
    ),
    Row(
        "list_remove_value_not_found",
        "fatal",
        "[1, 2].remove(3);",
        expected_message="Value not found in list.",
    ),
    Row(
        "undefined_method_on_list",
        "fatal",
        "[].bogus();",
        expected_message="Undefined method 'bogus' on list.",
    ),
    Row(
        "undefined_method_on_file",
        "fatal",
        'var f = open("__WORKDIR__/loxpp_fault_table_probe2.txt", "w"); f.write("x"); '
        'var g = open("__WORKDIR__/loxpp_fault_table_probe2.txt", "r"); g.bogus();',
        expected_message="Undefined method 'bogus' on file.",
    ),
    Row(
        "undefined_method_on_map",
        "fatal",
        "var m = {}; m.bogus();",
        expected_message="Undefined method 'bogus' on map.",
    ),
    Row(
        "superclass_not_a_class",
        "fatal",
        "var NotAClass = 1; class Sub < NotAClass {}",
        expected_message="Superclass must be a class.",
    ),
    Row(
        "super_method_not_found",
        "fatal",
        "class A {} class B < A { m() { super.zzz(); } } B().m();",
        expected_message="Undefined property 'zzz'.",
    ),
    # Missing feature (issue #349): bootstrap cannot index an ObjEnum value
    # at all, so it never reaches this fault's own message. `enum` is only
    # legal at global scope, so it goes in `setup`, not `body`.
    Row(
        "enum_index_type_error",
        "fatal",
        'v["a"];',
        setup="enum E { A(x) } var v = A(1);\n",
        expected_message="Enum field index must be a number.",
    ),
    Row(
        "enum_index_out_of_range",
        "fatal",
        "v[3];",
        setup="enum E { A(x) } var v = A(1);\n",
        expected_message="Enum field index 3 out of range.",
    ),
    Row(
        "string_index_assignment",
        "fatal",
        '"abc"[0] = "x";',
        expected_message="Strings are immutable and cannot be indexed for assignment.",
    ),
    Row(
        "slice_non_list_string",
        "fatal",
        "(42)[0:1];",
        expected_message="Slice requires a List or String.",
    ),
    Row(
        "slice_start_non_number",
        "fatal",
        '[1, 2]["a":2];',
        expected_message="Slice index must be a number.",
    ),
    Row(
        "slice_start_non_integer",
        "fatal",
        "[1, 2][1.5:2];",
        expected_message="Slice index must be an integer.",
    ),
    Row(
        "slice_start_negative",
        "fatal",
        "[1, 2][-1:2];",
        expected_message="Slice index must be non-negative.",
    ),
    Row(
        "slice_end_non_number",
        "fatal",
        '[1, 2][0:"a"];',
        expected_message="Slice index must be a number.",
    ),
    Row(
        "slice_end_non_integer",
        "fatal",
        "[1, 2][0:1.5];",
        expected_message="Slice index must be an integer.",
    ),
    Row(
        "slice_end_negative",
        "fatal",
        "[1, 2][0:-1];",
        expected_message="Slice index must be non-negative.",
    ),
    Row(
        "in_string_non_string_elem",
        "fatal",
        '1 in "abc";',
        expected_message="Left operand of 'in' on a string must be a string.",
    ),
    Row(
        "in_non_indexable",
        "fatal",
        "1 in 42;",
        expected_message="Right operand of 'in' must be a list, string, or map.",
    ),
    Row(
        "for_in_non_iterable",
        "fatal",
        "for (var x in 42) {}",
        expected_message="Value is not iterable (expected list, string, or map).",
    ),
    Row(
        "for_in_map_size_changed",
        "fatal",
        "var m = {1: 1}; for (var k in m) { m[2] = 2; }",
        expected_message="Map changed size during iteration.",
    ),
    # Regression rows for issue #348: bootstrap's fused property-access site
    # must remain fatal when the call target involves chained property gets
    # or grouping, since native's fused Op::INVOKE can only split the two cases
    # when the source is a direct property access (`obj.prop(args)`).
    Row(
        "invoke_chained_property_get",
        "fatal",
        "42.foo.bar();",
        expected_message="Only instances have properties.",
    ),
    Row(
        "invoke_grouped_property_get",
        "fatal",
        "(42.foo)();",
        expected_message="Only instances have properties.",
    ),
    # A `defer`red call holding a non-callable value must be fatal (native's
    # runDefers). The CLR backend fails to *compile* a variant of this shape
    # that closes over a caught `e` inside a nested function (issue found
    # during this node's own build; a plain local variable, not a capture,
    # reaches the same runtime fault on every consumer without hitting that
    # unrelated CLR emitter gap).
    # Issue #351: bootstrap neither faults nor catches anything here -- the
    # non-callable deferred value is silently never invoked. That is a
    # third, distinct shape from the general by-design pattern the other
    # fatal rows share (see _BOOTSTRAP_FATAL_DEFAULT_SKIP below): not
    # caught, not fatal, nothing reported at all.
    Row(
        "defer_noncallable_value",
        "fatal",
        "fun g() { var x = 42; defer x(); } g();",
        expected_message="Deferred callable has unexpected type.",
    ),
]

# --- Error-as-receiver reflection rows -------------------------------------
#
# spec/03-types.md's Error section (lines 223-225) makes a write to any name
# on an Error value a runtime error; the reflection natives are a second
# door onto the same restriction, separate from `.` property access. `e` is
# bound the same way in every row: a caught IndexOutOfBoundsError, captured
# by a top-level helper run in `setup`, before FATAL_TEMPLATE's own `try`.
# The helper's own try/catch closes itself, so the generated program keeps
# FATAL_TEMPLATE's `try { ... } catch (_) { ... }` shape intact for the row's
# fault-causing `body`.
_REFLECT_SETUP = (
    "fun __reflectErr() {\n"
    "    try { var x = []; print x[0]; } catch (e) { return e; }\n"
    "}\n"
    "var e = __reflectErr();\n"
    # Guards each reflection row against a corpus bug that binds `e` to
    # something other than a genuine Error: a wrong receiver here must be
    # visible as its own divergence, not silently pass a row whose natives
    # reject any non-instance the same way they reject an Error.
    'if (type(e) != "Error") { print "__UNREACHABLE__"; }\n'
)
REFLECT_ROWS = [
    Row(
        "reflect_fields_on_error",
        "fatal",
        "fields(e);",
        setup=_REFLECT_SETUP,
        expected_message="Expected an instance.",
    ),
    Row(
        "reflect_getfield_on_error",
        "fatal",
        'getField(e, "kind");',
        setup=_REFLECT_SETUP,
        expected_message="Only instances have properties.",
    ),
    Row(
        "reflect_hasfield_on_error",
        "fatal",
        'hasField(e, "kind");',
        setup=_REFLECT_SETUP,
        expected_message="Only instances have properties.",
    ),
    Row(
        "reflect_setfield_on_error",
        "fatal",
        'setField(e, "kind", 5);',
        setup=_REFLECT_SETUP,
        expected_message="Only instances have fields.",
    ),
    Row(
        "reflect_callmethod_on_error",
        "fatal",
        'callMethod(e, "foo");',
        setup=_REFLECT_SETUP,
        expected_message="Only instances have methods.",
    ),
]

# Reflection-on-Error is fatal on native, JVM, and CLR, but bootstrap's
# reflection natives deliver a catchable `ReflectionReceiverError` instead
# (issue #353) -- a real behavior gap, not a corpus bug, so every reflect
# row skips bootstrap rather than reporting the same divergence five times.
for _row in REFLECT_ROWS:
    _row.skip[BOOTSTRAP] = "issue #353: bootstrap makes reflection-on-Error catchable, not fatal"

ALL_ROWS = CATCHABLE_ROWS + FATAL_ROWS + REFLECT_ROWS

# --- Bootstrap's own disposition model -------------------------------------
#
# Node #335's own scope (issue #335) was "wire every setError() call site's
# `kind` string to the N1 table, no aliasing" -- never message-text parity,
# and never matching native's catchable/fatal split. Two facts, both
# already documented outside this script, follow from that scope:
#
# 1. Every catchable-table row's `kind` is proven correct by node #335's own
#    regression test (tools/check_bootstrap_error_kinds.py); this script
#    re-checks it here as a cross-consumer proof, but does not also demand
#    bootstrap's message text match native's -- that was never node #335's
#    deliverable. `type()`/`str()` are a separate, real gap (issue #347).
# 2. Bootstrap's tree-walker delivers EVERY Fatal Runtime Errors table cause
#    as a catchable Error, by design (see
#    tools/run_bootstrap_error_kind_causes.py's own module docstring) --
#    there is no fatal path on bootstrap for most of these rows to compare
#    against native's. Comparing bootstrap's outcome against native's fatal
#    disposition, row by row, would report ~30 "divergences" that are all
#    the same one architectural fact, not 30 distinct defects. The Error-
#    as-receiver reflection rows turned out to be the SAME architectural
#    fact, not an exception to it: running each one against bootstrap shows
#    it also delivers a catchable `ReflectionReceiverError`, not a halt (see
#    issue #353), so REFLECT_ROWS skips bootstrap too, for the same reason
#    as the rest of this table.
#
# A row that found a genuine, specific defect underneath this general
# pattern (a real kind ambiguity, a missing feature, a silently-swallowed
# fault) keeps its own specific `skip` reason and issue number instead of
# this default -- see property_get_non_instance, invoke_field_not_callable,
# enum_index_type_error, enum_index_out_of_range, and
# defer_noncallable_value above.
_BOOTSTRAP_FATAL_DEFAULT_SKIP = (
    "bootstrap delivers every Fatal Runtime Errors table cause as a catchable "
    "Error by design (see tools/run_bootstrap_error_kind_causes.py); node #335's "
    "own scope was kind-string correctness, not matching native's fatal "
    "disposition"
)
for _row in FATAL_ROWS:
    if _row.name == "stdlib_open_failure":
        # Unlike the rest of FATAL_ROWS, bootstrap agrees with native's
        # fatal disposition here (only the message text differs, already
        # handled by this row's own skip_fields), so it is exempt from the
        # blanket skip below.
        continue
    if _row.name == "defer_noncallable_value":
        # Node #351: bootstrap now faults on a deferred non-callable value,
        # matching native's fatal disposition, so it is exempt from the
        # blanket skip below.
        continue
    if _row.name == "enum_index_type_error":
        # Node #349: bootstrap now indexes enum values, matching native's
        # fatal disposition on type error, so it is exempt from the blanket
        # skip below.
        continue
    if _row.name == "enum_index_out_of_range":
        # Node #349: bootstrap now indexes enum values, matching native's
        # fatal disposition on out-of-range error, so it is exempt from the
        # blanket skip below.
        continue
    if _row.name == "property_get_non_instance":
        # Node #348: bootstrap now distinguishes plain property reads
        # (fatal) from method calls (catchable), so it is exempt from the
        # blanket skip below.
        continue
    if _row.name == "invoke_field_not_callable":
        # Node #348: bootstrap now distinguishes calls to field values
        # (fatal) from calls to literal values (catchable), so it is exempt
        # from the blanket skip below.
        continue
    if _row.name == "stdlib_native_arity":
        # Node #348: bootstrap now distinguishes stdlib native arity errors
        # (fatal) from user function arity errors (catchable), so it is
        # exempt from the blanket skip below.
        continue
    if _row.name == "list_append_wrong_arity":
        # Issue #365: bootstrap reports native's own custom text and halts,
        # so it is exempt from the blanket skip below.
        continue
    if _row.name == "list_pop_wrong_arity":
        # Issue #365: bootstrap reports native's own custom text and halts,
        # so it is exempt from the blanket skip below.
        continue
    if _row.name == "list_remove_wrong_arity":
        # Issue #365: bootstrap reports native's own custom text and halts,
        # so it is exempt from the blanket skip below.
        continue
    if _row.name == "invoke_chained_property_get":
        # Node #348: bootstrap's fused call site stays fatal through a
        # chained property get (`42.foo.bar()`), matching native, so it is
        # exempt from the blanket skip below.
        continue
    if _row.name == "invoke_grouped_property_get":
        # Node #348: bootstrap's fused call site stays fatal through a
        # grouping (`(42.foo)()`), matching native, so it is exempt from
        # the blanket skip below.
        continue
    _row.skip.setdefault(BOOTSTRAP, _BOOTSTRAP_FATAL_DEFAULT_SKIP)

for _row in CATCHABLE_ROWS:
    fields = _row.skip_fields.setdefault(BOOTSTRAP, set())
    # kind is compared (node #335's actual deliverable). type()/str() are
    # not: issue #347 tracks bootstrap's own Error identity gap. message
    # text is not either: issue #354 tracks bootstrap's message text
    # disagreeing with native's Runtime Errors table on 12 of 18 rows.
    fields.update({"message", "type", "str"})


def validate_row_anchors(rows: list[Row]) -> None:
    """Every row must carry the literal its disposition is checked against.

    A "caught" row with no expected_kind, or a "fatal" row with no
    expected_message, can still print MATCH: compare() only checks a literal
    against native's own output when the row supplies one. Failing loudly
    here, before any row runs, closes that entry point for every row at
    once instead of one row at a time as each is separately noticed.
    """
    for row in rows:
        if row.disposition == "caught" and row.expected_kind is None:
            print(
                f"check_fault_table.py: row {row.name!r} is 'caught' but has no "
                "expected_kind",
                file=sys.stderr,
            )
            sys.exit(2)
        if row.disposition == "fatal" and row.expected_message is None:
            print(
                f"check_fault_table.py: row {row.name!r} is 'fatal' but has no "
                "expected_message",
                file=sys.stderr,
            )
            sys.exit(2)


def load_spec_table_kind_count() -> int:
    """Counts the catchable table's data rows in spec/04-semantics.md.

    Not a full re-derivation of every row's text -- CATCHABLE_ROWS already
    hand-transcribes each row's kind and Example -- but a change to the
    table's row count (a row added or removed) with no matching change here
    fails loudly instead of silently checking a stale corpus.
    """
    text = SPEC_PATH.read_text(encoding="utf-8")
    start = text.index("## Runtime Errors")
    end = text.index("### Fatal Runtime Errors", start)
    section = text[start:end]
    rows = [
        line
        for line in section.splitlines()
        if line.startswith("|") and "---" not in line and "Cause" not in line
    ]
    return len(rows)


def load_spec_fatal_row_count() -> int:
    """Counts the Fatal Runtime Errors table's data rows in spec/04-semantics.md.

    Guards FATAL_ROWS against corpus drift the same way
    load_spec_table_kind_count guards CATCHABLE_ROWS -- a row added to or
    removed from the fatal table with no matching change here fails loudly
    instead of silently checking a stale corpus.
    """
    text = SPEC_PATH.read_text(encoding="utf-8")
    start = text.index("### Fatal Runtime Errors")
    end = text.index("## Enum Types", start)
    section = text[start:end]
    rows = [
        line
        for line in section.splitlines()
        if line.startswith("|") and "---" not in line and "Cause" not in line
    ]
    return len(rows)


# Exit code every consumer uses for a compile-time (parse) error, distinct
# from a run-time fault's own exit code, which differs by consumer (native
# 70, JVM 1, CLR 134 via an unhandled .NET exception). A row whose program
# never compiles proves nothing about the fault it names: classify() must
# tell the two apart, not read "no marker on stdout" as "fatal".
COMPILE_ERROR_EXIT_CODE = 65


@dataclass
class RunResult:
    # "caught", "caught_halted" (reached the catch block's markers but never
    # printed AFTER_MARKER -- the program halted before running to
    # completion, a real divergence from a "caught" disposition), "fatal",
    # "compile_error" (program did not compile -- a corpus bug, never a real
    # row outcome), or "crash" (neither marker seen although the program
    # compiled and ran -- see module docstring hazard).
    outcome: str
    kind: str | None = None
    message: str | None = None
    type_: str | None = None
    str_: str | None = None


class ConsumerCommand:
    def __init__(self, kind: str, argv: list[str]) -> None:
        self.kind = kind
        self.argv = argv

    def run(self, program_path: Path, timeout: float) -> subprocess.CompletedProcess:
        full_env = os.environ.copy()
        if self.kind == BOOTSTRAP:
            full_env["LANGUAGE"] = "LOXPP"
        return subprocess.run(
            self.argv + [str(program_path)],
            capture_output=True,
            text=True,
            timeout=timeout,
            env=full_env,
        )


def _first_stderr_line(stderr: str) -> str:
    for line in stderr.splitlines():
        stripped = line.strip()
        if stripped:
            return stripped
    return ""


def classify(returncode: int, stdout: str, stderr: str) -> RunResult:
    lines = stdout.splitlines()
    if CAUGHT_MARKER in lines:
        idx = lines.index(CAUGHT_MARKER)
        rest = lines[idx + 1 :]
        # A fatal-template run's catch block prints only CAUGHT_MARKER
        # (see FATAL_TEMPLATE) -- reaching it at all is itself the
        # divergence from native, whether or not AFTER_MARKER follows.
        # A caught-template run that never reaches AFTER_MARKER halted
        # somewhere between the catch block and the end of the program --
        # a real divergence from a row's "caught" disposition, not the same
        # outcome as a run that ran to completion.
        outcome = "caught" if AFTER_MARKER in lines else "caught_halted"
        if len(rest) >= 4:
            return RunResult(
                outcome=outcome, kind=rest[0], message=rest[1], type_=rest[2], str_=rest[3]
            )
        return RunResult(outcome=outcome)
    if UNREACHABLE_MARKER in lines or AFTER_MARKER in lines:
        # The fatal template printed UNREACHABLE_MARKER (the fault did not
        # fire at all -- a corpus bug, not a real "fatal" outcome) or a
        # caught-template run somehow reached AFTER_MARKER without its own
        # catch block markers (also a corpus bug: the fault never fired).
        return RunResult(outcome="crash")
    # No marker at all: the program halted before printing one. That alone
    # does not mean "fatal" -- a program that never compiled also prints no
    # marker. Every consumer here exits with COMPILE_ERROR_EXIT_CODE on a
    # parse failure, so that exit code, not the absence of a marker, is
    # what "fatal" requires.
    if returncode == COMPILE_ERROR_EXIT_CODE:
        return RunResult(outcome="compile_error", message=_first_stderr_line(stderr))
    return RunResult(outcome="fatal", message=_first_stderr_line(stderr))


def run_row(row: Row, commands: dict[str, ConsumerCommand], workdir: Path, timeout: float) -> dict[str, RunResult]:
    program_path = workdir / f"{row.name}.lox"
    program_path.write_text(row.program(workdir), encoding="utf-8")
    results: dict[str, RunResult] = {}
    for consumer, command in commands.items():
        if consumer in row.skip:
            continue
        try:
            proc = command.run(program_path, timeout)
        except subprocess.TimeoutExpired:
            results[consumer] = RunResult(outcome="crash")
            continue
        results[consumer] = classify(proc.returncode, proc.stdout, proc.stderr)
    return results


def compare(row: Row, native_result: RunResult, other: RunResult, consumer: str) -> list[str]:
    problems = []
    if other.outcome != native_result.outcome:
        problems.append(
            f"outcome: native={native_result.outcome} {consumer}={other.outcome}"
        )
        return problems
    if native_result.outcome == "fatal":
        if row.expected_message is not None and native_result.message != row.expected_message:
            problems.append(
                f"native's own message ({native_result.message!r}) does not match "
                f"the spec table's Message column ({row.expected_message!r}) -- "
                "corpus bug, fix the row"
            )
        # Fatal message text is compared as a substring, not equality: each
        # consumer wraps the same message in its own prefix/trailer (native
        # "[line N] in script", the JVM's "Exception in thread \"main\"
        # lox.LoxError: ", the CLR's ".NET Unhandled exception. Lox.LoxError:
        # " plus a stack trace).
        skip = row.skip_fields.get(consumer, set())
        if "message" not in skip:
            native_msg = native_result.message or ""
            other_msg = other.message or ""
            if not native_msg or native_msg not in other_msg:
                problems.append(f"message: native={native_msg!r} not found in {consumer}={other_msg!r}")
        return problems
    if native_result.outcome != "caught":
        return problems
    skip = row.skip_fields.get(consumer, set())
    if row.expected_kind is not None and native_result.kind != row.expected_kind:
        problems.append(
            f"native's own kind ({native_result.kind}) does not match the table's "
            f"expected kind ({row.expected_kind}) -- corpus bug, fix the row"
        )
    if "kind" not in skip and other.kind != native_result.kind:
        problems.append(f"kind: native={native_result.kind!r} {consumer}={other.kind!r}")
    if "message" not in skip and other.message != native_result.message:
        problems.append(f"message: native={native_result.message!r} {consumer}={other.message!r}")
    if "type" not in skip and other.type_ != native_result.type_:
        problems.append(f"type(): native={native_result.type_!r} {consumer}={other.type_!r}")
    if "str" not in skip and other.str_ != native_result.str_:
        problems.append(f"str(): native={native_result.str_!r} {consumer}={other.str_!r}")
    return problems


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--native", default=str(REPO_ROOT / "build" / "loxpp"))
    parser.add_argument("--jvm", default=str(REPO_ROOT / "tools" / "loxpp_jvm.sh"))
    parser.add_argument("--clr", default=str(REPO_ROOT / "tools" / "loxpp_clr.sh"))
    parser.add_argument("--bootstrap", default=str(REPO_ROOT / "bootstrap" / "lox_wrapper.sh"))
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument(
        "--only", default=None, help="run only rows whose name contains this substring (for debugging)"
    )
    args = parser.parse_args()

    commands = {
        NATIVE: ConsumerCommand(NATIVE, [args.native]),
        JVM: ConsumerCommand(JVM, [args.jvm]),
        CLR: ConsumerCommand(CLR, [args.clr]),
        BOOTSTRAP: ConsumerCommand(BOOTSTRAP, [args.bootstrap]),
    }

    validate_row_anchors(ALL_ROWS)

    # MaxDepthExceededError is the one catchable-table row this script does
    # not run at all (see CATCHABLE_ROWS's own comment): its disposition
    # conflicts with the Fatal Runtime Errors table's own row for the same
    # fault, and issue #338, not this test, decides which is correct.
    excluded_catchable_rows = 1
    # call_non_callable_after_field_read is a second CATCHABLE_ROWS entry for
    # the same spec row as not_callable_error: regression armor for issue
    # #348 (a bare 42() must stay catchable even after an earlier field
    # read), not a new spec table row. It must not count against the 1:1
    # mapping this invariant checks between CATCHABLE_ROWS and spec rows.
    extra_catchable_rows = 1
    spec_row_count = load_spec_table_kind_count()
    if spec_row_count != len(CATCHABLE_ROWS) - extra_catchable_rows + excluded_catchable_rows:
        print(
            f"check_fault_table.py: spec/04-semantics.md's catchable table has "
            f"{spec_row_count} row(s), but CATCHABLE_ROWS covers "
            f"{len(CATCHABLE_ROWS)} (-{extra_catchable_rows} regression-only entry "
            f"for an existing row, +{excluded_catchable_rows} deliberately "
            "excluded, see MaxDepthExceededError's comment). The table changed; "
            "update this script's corpus to match.",
            file=sys.stderr,
        )
        sys.exit(2)

    # The canonical-string depth row is the one Fatal table row this script
    # does not run at all (issue #338 owns its disposition; see FATAL_ROWS's
    # own module comment). Guards FATAL_ROWS against the table changing
    # underneath it the same way the check above guards CATCHABLE_ROWS.
    excluded_fatal_rows = 1
    # invoke_chained_property_get and invoke_grouped_property_get are
    # regression armor for issue #348: they exercise the same spec row as
    # property_get_non_instance ("42.foo;" -- "Only instances have
    # properties.") through a chained-get and a grouping shape instead of a
    # bare name, to pin the fused call site's syntactic dispatch. Neither is
    # a new spec table row, so both must not count against the 1:1 mapping
    # this invariant checks between FATAL_ROWS and spec rows.
    extra_fatal_rows = 2
    spec_fatal_row_count = load_spec_fatal_row_count()
    if spec_fatal_row_count != len(FATAL_ROWS) - extra_fatal_rows + excluded_fatal_rows:
        print(
            f"check_fault_table.py: spec/04-semantics.md's Fatal Runtime Errors "
            f"table has {spec_fatal_row_count} row(s), but FATAL_ROWS covers "
            f"{len(FATAL_ROWS)} (-{extra_fatal_rows} regression-only entries for "
            f"an existing row, +{excluded_fatal_rows} deliberately excluded, "
            "see the canonical-string depth row's comment). The table changed; "
            "update this script's corpus to match.",
            file=sys.stderr,
        )
        sys.exit(2)

    rows = ALL_ROWS
    if args.only:
        rows = [r for r in rows if args.only in r.name]
        if not rows:
            print(f"check_fault_table.py: no row matches --only {args.only!r}", file=sys.stderr)
            sys.exit(2)

    diverged = 0
    skipped_rows = 0
    per_consumer_skips = {JVM: 0, CLR: 0, BOOTSTRAP: 0}
    with tempfile.TemporaryDirectory(prefix="loxpp_fault_table_") as tmp:
        workdir = Path(tmp)
        for row in rows:
            results = run_row(row, commands, workdir, args.timeout)
            native_result = results.get(NATIVE)
            if native_result is None:
                print(f"SKIP        {row.name}  (native itself is skipped for this row)")
                skipped_rows += 1
                continue
            if native_result.outcome == "compile_error":
                # Native itself never reached the fault: the row's program
                # is broken, not any consumer's behavior. Report it as a
                # failure rather than silently passing every consumer.
                print(
                    f"ERROR       {row.name}  native does not compile this row's "
                    f"program: {native_result.message!r} -- corpus bug, fix the row"
                )
                diverged += 1
                continue
            if native_result.outcome != row.disposition:
                # The row's declared disposition is native's own ground
                # truth (see the module docstring). Compare it against
                # native's actual outcome so a later change in native's own
                # behavior (a Fatal row native starts to catch, or the
                # reverse) shows up as a divergence instead of silently
                # matching every other consumer against a stale label.
                print(
                    f"DIVERGE     {row.name}  [native]  disposition: row declares "
                    f"{row.disposition!r} but native's own outcome is "
                    f"{native_result.outcome!r}"
                )
                diverged += 1
                continue

            row_ok = True
            row_notes = []
            for consumer in (JVM, CLR, BOOTSTRAP):
                if consumer in row.skip:
                    row_notes.append(f"{consumer} SKIPPED ({row.skip[consumer]})")
                    per_consumer_skips[consumer] += 1
                    continue
                other_result = results[consumer]
                problems = compare(row, native_result, other_result, consumer)
                if problems:
                    row_ok = False
                    for problem in problems:
                        print(f"DIVERGE     {row.name}  [{consumer}]  {problem}")
                    diverged += 1

            if row_ok:
                suffix = f"  ({'; '.join(row_notes)})" if row_notes else ""
                print(f"MATCH       {row.name}  ({native_result.outcome}){suffix}")

    print()
    print(f"{len(rows) - skipped_rows} row(s) checked, {skipped_rows} row(s) not runnable, {diverged} divergence(s)")
    print(
        "per-consumer row skips: "
        + ", ".join(f"{consumer}={count}" for consumer, count in per_consumer_skips.items())
    )
    sys.exit(1 if diverged else 0)


if __name__ == "__main__":
    main()
