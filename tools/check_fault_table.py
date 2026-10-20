#!/usr/bin/env python3
"""
Differential fault test across three consumers: native (build/loxpp), the
JVM backend (tools/loxpp_jvm.sh), and the bootstrap interpreter
(bootstrap/lox_wrapper.sh).

For every row of spec/04-semantics.md's Runtime Errors table (the catchable
faults) and its Halting Faults table, plus the five Error-as-receiver
reflection rows that spec/03-types.md's Error section implies, this script
runs one small Lox++ program on each consumer. It checks the rules of
spec/README.md, Conformance:

  - A catchable row: the fault reaches the catchBlock on every consumer,
    with the same `e.kind` and `type(e)`. `e.message` and `str(e)` are not
    compared: their text is implementation-defined.
  - A halting row: the fault occurs on every consumer. The program halts
    with a non-zero exit status, or, because an implementation may make a
    halting fault catchable, the catchBlock receives it. A consumer that
    goes on past the fault without either outcome fails.

Native's own outcome must also be the one the row declares: native halts on
every halting row.

A row's `skip` set leaves a consumer out of one row. It names the open issue
that tracks the defect.

Usage:
    tools/check_fault_table.py [--native PATH] [--jvm PATH]
                                [--bootstrap PATH] [--timeout SECONDS]

Exits 0 when every non-skipped (row, consumer) pair conforms. Exits 1 and
prints every mismatch otherwise. Exits 2 and names the problem when an
input is unusable: a missing JVM runtime jar, a stale row corpus, or a bad
--only value.
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
JVM_WRAPPER = REPO_ROOT / "tools" / "loxpp_jvm.sh"
DEFAULT_JVM_RT_JAR = REPO_ROOT / "runtime" / "jvm" / "lox-rt.jar"

NATIVE = "native"
JVM = "jvm"
BOOTSTRAP = "bootstrap"

# A row's program prints these markers (never "kind"/"type" text that a
# fault's report could itself contain) so a run's outcome is read from
# a fixed vocabulary, not guessed from arbitrary program output.
CAUGHT_MARKER = "__CAUGHT__"
AFTER_MARKER = "__AFTER__"
UNREACHABLE_MARKER = "__UNREACHABLE__"

CAUGHT_TEMPLATE = """{setup}try {{
    {body}
}} catch (e) {{
    print "{caught}";
    print e.kind;
    print type(e);
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
    skip: dict[str, str] = field(default_factory=dict)  # consumer -> reason, skips the whole row

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


# --- Catchable Runtime Errors table (spec/04-semantics.md, 18 rows) -------
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
    # A deferred call's own arity mismatch (issue #359): the same ArityError
    # kind as an ordinary call, through VM::runPendingDefers -> VM::call, not
    # a new spec table row.
    Row(
        "arity_error_deferred_call",
        "caught",
        "g();",
        setup="fun needsOne(a) {}\nfun g() { defer needsOne(); }\n",
        expected_kind="ArityError",
    ),
    # Issue #405: a deferred call's own arity mismatch, found while draining
    # defers for a frame that is ALREADY unwinding an unrelated throw --
    # native always lets the newly found fault replace whichever throw was
    # in flight, handler present or not. Same ArityError kind and spec row
    # as arity_error_deferred_call above, through a different program shape.
    Row(
        "arity_error_deferred_call_during_throw",
        "caught",
        "g();",
        setup='fun needsOne(a) {}\nfun g() { defer needsOne(); throw "boom"; }\n',
        expected_kind="ArityError",
    ),
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
        setup="fun f() { return 1 + f(); }\n",
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
    # Regression rows for issue #371: spec/04-semantics.md's Index Get section
    # and these same three table rows now cover a String receiver too, not
    # only a List -- these three rows pin that a String receiver gets the
    # identical kind a List receiver gets, on every consumer.
    Row("index_type_error_string", "caught", 's["a"];', setup='var s = "abc";\n', expected_kind="IndexTypeError"),
    Row(
        "index_not_integer_error_string",
        "caught",
        "s[1.5];",
        setup='var s = "abc";\n',
        expected_kind="IndexNotIntegerError",
    ),
    Row(
        "index_out_of_bounds_error_string",
        "caught",
        "s[10];",
        setup='var s = "abc";\n',
        expected_kind="IndexOutOfBoundsError",
    ),
    Row("empty_list_error", "caught", "[].pop();", expected_kind="EmptyListError"),
    Row("nan_key_error", "caught", "m[0/0] = 1;", setup="var m = {};\n", expected_kind="NaNKeyError"),
    Row(
        "invalid_map_key_error",
        "caught",
        "m[[1,2]] = 1;",
        setup="var m = {};\n",
        expected_kind="InvalidMapKeyError",
    ),
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
    # Regression row: a class with no init() has arity 0 (this row's own
    # `expected_kind` above covers only the enum-constructor shape). Found
    # while adding issue #367's stdout-identity check to
    # tools/check_bootstrap_stack_overflow.py, which surfaced
    # examples/try_catch_class_constructor_arity.lox aliasing this to plain
    # ArityError on bootstrap.
    Row(
        "constructor_arity_error_no_init_class",
        "caught",
        "C(1, 2);",
        setup="class C {}\n",
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
    # Operator overloading result validation (issue #472): an operator method
    # whose result must be a Boolean returned something else.
    Row(
        "operator_result_type_error",
        "caught",
        "C() == C();",
        setup="class C { __eq__(o) { return 42; } }\n",
        expected_kind="OperatorResultTypeError",
    ),
    # Reflected operator result validation (issue #508): the right operand's
    # reflected comparison method returned a non-Boolean.
    Row(
        "reflected_operator_result_type_error",
        "caught",
        "1 < C();",
        setup="class C { __rlt__(o) { return 42; } }\n",
        expected_kind="OperatorResultTypeError",
    ),
    # An operator method whose result must be a Number returned something else
    # (issue #469; #472 added the Boolean case above).
    Row(
        "operator_result_type_error_number",
        "caught",
        "len(C());",
        setup='class C { __len__() { return "x"; } }\n',
        expected_kind="OperatorResultTypeError",
    ),
    # A key method that writes the map it keys (issue #469): the map is locked
    # during hashing and probing, so the write is a catchable error.
    Row(
        "map_changed_during_key_op",
        "caught",
        "m[K()] = 1;",
        setup="var m = {};\nclass K { __hash__() { m[1] = 1; return 1; } "
              "__eq__(o) { return true; } }\n",
        expected_kind="MapChangedError",
    ),
    # --- Coroutines (roadmap item 5, tracking #523) ---------------------
    # The coroutine protocol faults. Every consumer runs them.
    Row(
        "resume_dead_coroutine",
        "caught",
        "co.resume();",
        setup="fun f() { return 1; }\nvar co = coroutine.create(f);\nco.resume();\n",
        expected_kind="DeadCoroutineError",
    ),
    Row(
        "resume_running_coroutine",
        "caught",
        "co.resume();",
        setup="fun f() { co.resume(); }\nvar co = coroutine.create(f);\n",
        expected_kind="RunningCoroutineError",
    ),
    Row(
        "yield_across_native_callback",
        "caught",
        "co.resume();",
        setup='class S { init() {} __str__() { yield 1; return "x"; } }\n'
              'fun f() { print S(); return "done"; }\n'
              "var co = coroutine.create(f);\n",
        expected_kind="YieldAcrossNativeError",
    ),
    # A yield in a deferred call (spec/04-semantics.md, yield Expression): the
    # drain raises, in the main task outside every coroutine too.
    Row(
        "yield_in_deferred_call",
        "caught",
        "g();",
        setup="fun note() { var v = yield 1; return v; }\n"
              "fun g() { defer note(); print \"g body\"; }\n",
        expected_kind="YieldAcrossNativeError",
    ),
]

# --- Halting Faults table (spec/04-semantics.md) --------------------------
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
    ),
    Row(
        "max_depth_exceeded",
        "fatal",
        "print a;",
        setup="var a = [1]; var i = 0; while (i < 300) { a = [a]; i = i + 1; }\n",
    ),
    Row(
        "stdlib_native_arity",
        "fatal",
        "clock(1);",
    ),
    Row(
        "stdlib_open_failure",
        "fatal",
        'open("/no/such/path", "r");',
    ),
    # Issue #375: unlike an index read/write, a map/set literal, or `in`
    # (the catchable table's NaNKeyError/InvalidMapKeyError rows above),
    # Map.has/Map.del are stdlib native methods, and reach this same
    # cause through the stdlib-native error path -- a halting fault,
    # regardless of NaN vs. a non-String object key. A
    # pre-existing native-VM asymmetry between the opcode-level and
    # stdlib-native forms of the same cause, not a defect in one consumer.
    Row(
        "map_has_invalid_key",
        "fatal",
        "m.has([1, 2]);",
        setup="var m = {};\n",
    ),
    # Regression armor: map_has_invalid_key's own spec row ("Map.has(key)
    # or Map.del(key)") covers both natives with one Example; this row
    # pins Map.del specifically, not a second spec table row.
    Row(
        "map_del_invalid_key",
        "fatal",
        "m.del(0.0/0.0);",
        setup="var m = {};\n",
    ),
    Row(
        "undefined_property_on_file",
        "fatal",
        'var f = open("__WORKDIR__/loxpp_fault_table_probe.txt", "w"); f.write("x"); '
        'var g = open("__WORKDIR__/loxpp_fault_table_probe.txt", "r"); g.bogus;',
    ),
    Row(
        "undefined_property_on_socket",
        "fatal",
        'var l = listen("127.0.0.1", 0); var s = connect("127.0.0.1", l.port()); s.bogus;',
    ),
    Row(
        "undefined_property_on_server",
        "fatal",
        'var l = listen("127.0.0.1", 0); l.bogus;',
    ),
    Row(
        "undefined_property_on_process",
        "fatal",
        'var p = spawn("echo", []); p.bogus;',
    ),
    Row(
        "undefined_property_on_map",
        "fatal",
        "var m = {}; m.bogus;",
    ),
    Row(
        "undefined_property_on_instance",
        "fatal",
        "class C {} var c = C(); c.bogus;",
    ),
    # Issue #748: native compiles a class pattern's braced name as
    # GET_PROPERTY, so the same spec row applies inside a class pattern.
    Row(
        "undefined_property_in_class_pattern",
        "fatal",
        "match C() { case C{bogus} => 1 case _ => 0 };",
        setup="class C {}\n",
    ),
    # Issue #748: native compiles object destructuring as GET_PROPERTY too,
    # so a missing name is the same halting fault (spec/04-semantics.md,
    # Destructuring Declaration).
    Row(
        "undefined_property_in_destructure",
        "fatal",
        "var {bogus} = C();",
        setup="class C {}\n",
    ),
    # Issue #748: a Map is in Property Get steps 2-5, so destructuring a
    # missing Map method is the Map's own halting fault.
    Row(
        "undefined_property_in_map_destructure",
        "fatal",
        "var {bogus} = m;",
        setup="var m = {};\n",
    ),
    # Issue #748: a non-instance destructuring source is a property read on a
    # non-Instance. This row checks that the fault occurs. The table accepts a
    # catchable fault, so probe 122 checks that it is not catchable.
    Row(
        "destructure_non_instance",
        "fatal",
        "var {x} = 42;",
    ),
    # Fused site (issue #348): bootstrap's tree-walker used to give this
    # the same catchable kind it gives `42.foo()` (a call), because it could
    # not tell a plain property-get apart from a call target. Now it
    # distinguishes them by tracking whether property access is a call target.
    Row(
        "property_get_non_instance",
        "fatal",
        "42.foo;",
    ),
    # Issue #707: the same spec row, on a List. A List is not in Property
    # Get steps 2-5, so a read of a List method is a property read on a
    # non-Instance. A direct call stays legal; the defer row below covers
    # the other read path.
    Row(
        "list_method_read",
        "fatal",
        "l.append;",
        setup="var l = [1];\n",
    ),
    # Issue #674: the callee of `defer` is evaluated as steps 1-2 of
    # Function Call, so the same read faults at the defer statement on a
    # List method.
    Row(
        "list_method_read_defer",
        "fatal",
        "g();",
        setup="var l = [];\nfun g() { defer l.append(1); }\n",
    ),
    Row(
        "property_set_non_instance",
        "fatal",
        "42.foo = 1;",
    ),
    # Fused site (issue #348): bootstrap now distinguishes property-access
    # sources, making calls to non-callable field values fatal while keeping
    # calls to non-callable literal values catchable.
    Row(
        "invoke_field_not_callable",
        "fatal",
        "class C { init() { this.f = 1; } } C().f();",
    ),
    Row(
        "invoke_method_not_found",
        "fatal",
        "class C {} C().bogus();",
    ),
    Row(
        "list_append_wrong_arity",
        "fatal",
        "[].append();",
    ),
    Row(
        "list_pop_wrong_arity",
        "fatal",
        "[].pop(1);",
    ),
    Row(
        "list_remove_wrong_arity",
        "fatal",
        "[1].remove();",
    ),
    Row(
        "list_remove_value_not_found",
        "fatal",
        "[1, 2].remove(3);",
    ),
    Row(
        "undefined_method_on_list",
        "fatal",
        "[].bogus();",
    ),
    Row(
        "undefined_method_on_file",
        "fatal",
        'var f = open("__WORKDIR__/loxpp_fault_table_probe2.txt", "w"); f.write("x"); '
        'var g = open("__WORKDIR__/loxpp_fault_table_probe2.txt", "r"); g.bogus();',
    ),
    Row(
        "undefined_method_on_socket",
        "fatal",
        'var l = listen("127.0.0.1", 0); var s = connect("127.0.0.1", l.port()); s.bogus();',
    ),
    Row(
        "undefined_method_on_server",
        "fatal",
        'var l = listen("127.0.0.1", 0); l.bogus();',
    ),
    Row(
        "undefined_method_on_process",
        "fatal",
        'var p = spawn("echo", []); p.bogus();',
    ),
    Row(
        "undefined_method_on_map",
        "fatal",
        "var m = {}; m.bogus();",
    ),
    Row(
        "superclass_not_a_class",
        "fatal",
        "var NotAClass = 1; class Sub < NotAClass {}",
    ),
    Row(
        "super_method_not_found",
        "fatal",
        "class A {} class B < A { m() { super.zzz(); } } B().m();",
    ),
    # `enum` is only legal at global scope, so it goes in `setup`, not `body`.
    Row(
        "enum_index_type_error",
        "fatal",
        'v["a"];',
        setup="enum E { A(x) } var v = A(1);\n",
    ),
    Row(
        "enum_index_out_of_range",
        "fatal",
        "v[3];",
        setup="enum E { A(x) } var v = A(1);\n",
    ),
    # Regression armor for issue #363: a NaN/infinite/oversized index has no
    # well-defined narrowing conversion to int on any consumer (C++
    # static_cast, Java narrowing conversion, C# unchecked (int) cast are
    # all unspecified/UB for these inputs) and, on JVM's NaN case, used to
    # land the conversion's own zero result in bounds and skip the fault
    # entirely.
    Row(
        "enum_index_nan",
        "fatal",
        "v[0.0/0.0];",
        setup="enum E { A(x) } var v = A(1);\n",
    ),
    Row(
        "enum_index_infinity",
        "fatal",
        "v[1.0/0.0];",
        setup="enum E { A(x) } var v = A(1);\n",
    ),
    Row(
        "enum_index_oversized",
        "fatal",
        "v[99999999999.0];",
        setup="enum E { A(x) } var v = A(1);\n",
    ),
    Row(
        "string_index_assignment",
        "fatal",
        '"abc"[0] = "x";',
    ),
    Row(
        "slice_non_list_string",
        "fatal",
        "(42)[0:1];",
    ),
    Row(
        "slice_start_non_number",
        "fatal",
        '[1, 2]["a":2];',
    ),
    Row(
        "slice_start_non_integer",
        "fatal",
        "[1, 2][1.5:2];",
    ),
    Row(
        "slice_start_negative",
        "fatal",
        "[1, 2][-1:2];",
    ),
    Row(
        "slice_end_non_number",
        "fatal",
        '[1, 2][0:"a"];',
    ),
    Row(
        "slice_end_non_integer",
        "fatal",
        "[1, 2][0:1.5];",
    ),
    Row(
        "slice_end_negative",
        "fatal",
        "[1, 2][0:-1];",
    ),
    Row(
        "in_string_non_string_elem",
        "fatal",
        '1 in "abc";',
    ),
    Row(
        "in_non_indexable",
        "fatal",
        "1 in 42;",
    ),
    Row(
        "for_in_non_iterable",
        "fatal",
        "for (var x in 42) {}",
    ),
    Row(
        "for_in_map_size_changed",
        "fatal",
        "var m = {1: 1}; for (var k in m) { m[2] = 2; }",
    ),
    # Regression rows for issue #348: bootstrap's fused property-access site
    # must remain fatal when the call target involves chained property gets
    # or grouping, since native's fused Op::INVOKE can only split the two cases
    # when the source is a direct property access (`obj.prop(args)`).
    Row(
        "invoke_chained_property_get",
        "fatal",
        "42.foo.bar();",
    ),
    Row(
        "invoke_grouped_property_get",
        "fatal",
        "(42.foo)();",
    ),
    # A `defer`red call holding a non-callable value must be fatal (native's
    # runDefers).
    # Issue #351: bootstrap neither faults nor catches anything here -- the
    # non-callable deferred value is silently never invoked. That is a
    # third, distinct shape from the general by-design pattern the other
    # fatal rows share (see _BOOTSTRAP_FATAL_DEFAULT_SKIP below): not
    # caught, not fatal, nothing reported at all.
    Row(
        "defer_noncallable_value",
        "fatal",
        "fun g() { var x = 42; defer x(); } g();",
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
    ),
    Row(
        "reflect_getfield_on_error",
        "fatal",
        'getField(e, "kind");',
        setup=_REFLECT_SETUP,
    ),
    Row(
        "reflect_hasfield_on_error",
        "fatal",
        'hasField(e, "kind");',
        setup=_REFLECT_SETUP,
    ),
    Row(
        "reflect_setfield_on_error",
        "fatal",
        'setField(e, "kind", 5);',
        setup=_REFLECT_SETUP,
    ),
    # Issue #355: the plain `.` write door onto the same restriction --
    # spec/03-types.md's Error section makes a write to any name on an
    # Error value a runtime error, and the reflection door above (setField)
    # is not the only way to reach it.
    Row(
        "property_set_on_error",
        "fatal",
        'e.kind = "z";',
        setup=_REFLECT_SETUP,
    ),
    Row(
        "reflect_callmethod_on_error",
        "fatal",
        'callMethod(e, "foo");',
        setup=_REFLECT_SETUP,
    ),
    # A non-instance, non-Error receiver (a Number here) is fatal the same
    # way an Error receiver is (issue #370: bootstrap special-cased only the
    # Error receiver, every other non-instance receiver stayed catchable).
    Row(
        "reflect_fields_on_number",
        "fatal",
        "fields(42);",
    ),
    Row(
        "reflect_methods_on_number",
        "fatal",
        "methods(42);",
    ),
    Row(
        "reflect_getfield_on_number",
        "fatal",
        'getField(42, "x");',
    ),
    Row(
        "reflect_hasfield_on_number",
        "fatal",
        'hasField(42, "x");',
    ),
    Row(
        "reflect_setfield_on_number",
        "fatal",
        'setField(42, "x", 1);',
    ),
    Row(
        "reflect_callmethod_on_number",
        "fatal",
        'callMethod(42, "foo");',
    ),
]

ALL_ROWS = CATCHABLE_ROWS + FATAL_ROWS + REFLECT_ROWS

# --- Named regression-only rows (issue #368) --------------------------------
#
# Each entry here duplicates an existing spec table row under a different
# shape (a different receiver, a different edge-case value) to pin a
# specific past bug, not to cover a new spec row. Held by NAME, not by a bare
# count: `main()`'s row-count gate below checks that these exact names are
# present in CATCHABLE_ROWS/FATAL_ROWS, in addition to the arithmetic (spec
# row count + this set's size == the table's row count). A count-only check
# cannot tell "these three known regression rows are present" from "three
# different, unbacked rows were swapped in" -- the totals still agree either
# way. Checking the names closes that gap: a swap changes which names are
# present even when the total does not change.
CATCHABLE_REGRESSION_ROWS = {
    # Issue #348: a bare 42() must stay catchable even after an earlier
    # field read (regression armor for the same spec row as
    # not_callable_error).
    "call_non_callable_after_field_read",
    # Issue #371: a String receiver shares its spec row with the matching
    # List row, not a row of its own.
    "index_type_error_string",
    "index_not_integer_error_string",
    "index_out_of_bounds_error_string",
    # Found while working issue #367: a second entry for the same spec row
    # as constructor_arity_error, pinning the no-init-class aliasing bug.
    "constructor_arity_error_no_init_class",
    # Issue #359: a deferred call's own arity mismatch shares arity_error's
    # spec row, through a different call site (VM::runPendingDefers).
    "arity_error_deferred_call",
    # Issue #405: same cause, found while draining defers during an
    # already-in-flight throw instead of a normal return.
    "arity_error_deferred_call_during_throw",
    # Issue #508: the right operand's reflected comparison method returned a
    # non-Boolean, sharing operator_result_type_error's spec row.
    "reflected_operator_result_type_error",
}

FATAL_REGRESSION_ROWS = {
    # Issue #348: pins the fused call site's syntactic dispatch through a
    # chained-get and a grouping shape, same spec row as
    # property_get_non_instance.
    "invoke_chained_property_get",
    "invoke_grouped_property_get",
    # Issue #363: same spec row as enum_index_out_of_range, with a NaN,
    # infinite, or oversized index instead of an ordinary in-range one.
    "enum_index_nan",
    "enum_index_infinity",
    "enum_index_oversized",
    # Issue #375: map_del_invalid_key exercises the same spec row as
    # map_has_invalid_key through Map.del instead of Map.has, to pin both
    # natives, not a second spec table row.
    "map_del_invalid_key",
    # Issues #674, #707: both rows exercise property_get_non_instance's spec
    # row on a List method, once as a plain read and once as a defer callee.
    "list_method_read",
    "list_method_read_defer",
    # Issue #748: a class pattern and an object destructuring both read a
    # property through GET_PROPERTY, so they share the spec rows of
    # undefined_property_on_instance and property_get_non_instance.
    "undefined_property_in_class_pattern",
    "undefined_property_in_destructure",
    "undefined_property_in_map_destructure",
    "destructure_non_instance",
}


def check_named_regression_rows(rows: list[Row], regression_names: set[str], table_label: str) -> list[str]:
    """Returns failure strings (empty if none): every name in
    `regression_names` must actually be a row in `rows`, by name -- not just
    a count match. See CATCHABLE_REGRESSION_ROWS's own comment for why a bare
    count cannot catch a swapped-in unbacked row."""
    present = {r.name for r in rows}
    missing = regression_names - present
    if not missing:
        return []
    return [
        f"check_fault_table.py: {table_label}'s declared regression-only row(s) "
        f"{sorted(missing)!r} are not present by name -- a row was renamed or "
        "removed without updating CATCHABLE_REGRESSION_ROWS/FATAL_REGRESSION_ROWS."
    ]


def validate_row_anchors(rows: list[Row]) -> None:
    """Every "caught" row must carry the kind it is checked against.

    A "caught" row with no expected_kind could still print MATCH, because
    compare() checks the kind only when the row supplies one.
    """
    for row in rows:
        if row.disposition == "caught" and row.expected_kind is None:
            print(
                f"check_fault_table.py: row {row.name!r} is 'caught' but has no "
                "expected_kind",
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
    end = text.index("### Halting Faults", start)
    section = text[start:end]
    rows = [
        line
        for line in section.splitlines()
        if line.startswith("|") and "---" not in line and "Cause" not in line
    ]
    return len(rows)


def load_spec_fatal_row_count() -> int:
    """Counts the Halting Faults table's data rows in spec/04-semantics.md.

    Guards FATAL_ROWS against corpus drift the same way
    load_spec_table_kind_count guards CATCHABLE_ROWS -- a row added to or
    removed from the fatal table with no matching change here fails loudly
    instead of silently checking a stale corpus.
    """
    text = SPEC_PATH.read_text(encoding="utf-8")
    start = text.index("### Halting Faults")
    end = text.index("## Enum Types", start)
    section = text[start:end]
    rows = [
        line
        for line in section.splitlines()
        if line.startswith("|") and "---" not in line and "Cause" not in line
    ]
    return len(rows)


# Exit code that every consumer here uses for a rejected program. A row whose
# program never compiles proves nothing about the fault it names: classify()
# must tell the two apart, not read "no marker on stdout" as "fatal". The
# spec requires only a non-zero status; the three consumers all use 65.
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
    returncode: int | None = None
    kind: str | None = None
    type_: str | None = None
    # the first stderr line, printed for a corpus bug; never compared
    report: str | None = None


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


def require_jvm_rt_jar(jvm: str) -> None:
    """Exits 2 when the default JVM wrapper's runtime jar is absent.

    tools/loxpp_jvm.sh exits non-zero with no program output when the jar is
    missing, and classify() reads that as a fault: every catchable row then
    reports a divergence against a consumer that never started. A custom
    --jvm command owns its own runtime and startup, so only the default
    wrapper is checked.
    """
    if Path(jvm).resolve() != JVM_WRAPPER.resolve():
        return
    jar = Path(os.environ.get("LOX_RT_JAR") or DEFAULT_JVM_RT_JAR)
    if not jar.is_file():
        print(
            f"check_fault_table.py: no JVM runtime jar at {jar}; "
            "run tools/build_lox_rt.sh first",
            file=sys.stderr,
        )
        sys.exit(2)


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
        if len(rest) >= 2:
            return RunResult(outcome=outcome, kind=rest[0], type_=rest[1])
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
        return RunResult(outcome="compile_error", report=_first_stderr_line(stderr))
    return RunResult(outcome="fatal", returncode=returncode, report=_first_stderr_line(stderr))


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
    if row.disposition == "fatal":
        # A halting fault: a halt with a non-zero status conforms, and so does
        # a catch that runs to the end, because an implementation may make a
        # halting fault catchable (spec/README.md, Conformance).
        if other.outcome == "fatal":
            if other.returncode == 0:
                return [f"exit status: {consumer} halted with status 0"]
            return []
        if other.outcome == "caught":
            return []
        return [f"outcome: {consumer}={other.outcome}; a halting fault must halt or be caught"]
    problems = []
    if other.outcome != native_result.outcome:
        problems.append(
            f"outcome: native={native_result.outcome} {consumer}={other.outcome}"
        )
        return problems
    if row.expected_kind is not None and native_result.kind != row.expected_kind:
        problems.append(
            f"native's own kind ({native_result.kind}) does not match the table's "
            f"expected kind ({row.expected_kind}) -- corpus bug, fix the row"
        )
    if other.kind != native_result.kind:
        problems.append(f"kind: native={native_result.kind!r} {consumer}={other.kind!r}")
    if other.type_ != native_result.type_:
        problems.append(f"type(): native={native_result.type_!r} {consumer}={other.type_!r}")
    return problems


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--native", default=str(REPO_ROOT / "build" / "loxpp"))
    parser.add_argument("--jvm", default=str(REPO_ROOT / "tools" / "loxpp_jvm.sh"))
    parser.add_argument("--bootstrap", default=str(REPO_ROOT / "bootstrap" / "lox_wrapper.sh"))
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument(
        "--only", default=None, help="run only rows whose name contains this substring (for debugging)"
    )
    args = parser.parse_args()
    require_jvm_rt_jar(args.jvm)

    commands = {
        NATIVE: ConsumerCommand(NATIVE, [args.native]),
        JVM: ConsumerCommand(JVM, [args.jvm]),
        BOOTSTRAP: ConsumerCommand(BOOTSTRAP, [args.bootstrap]),
    }

    validate_row_anchors(ALL_ROWS)

    # See CATCHABLE_REGRESSION_ROWS/FATAL_REGRESSION_ROWS's own comment
    # (issue #368): each gate below checks both the arithmetic (spec row
    # count + the named regression set's size == the table's row count) and
    # that every declared regression-only name is actually present in the
    # table, by name -- not a bare count, which cannot tell a declared
    # regression row from an unrelated unbacked row that happens to keep the
    # total the same.
    gate_failures: list[str] = []
    spec_row_count = load_spec_table_kind_count()
    if spec_row_count != len(CATCHABLE_ROWS) - len(CATCHABLE_REGRESSION_ROWS):
        gate_failures.append(
            f"check_fault_table.py: spec/04-semantics.md's catchable table has "
            f"{spec_row_count} row(s), but CATCHABLE_ROWS covers "
            f"{len(CATCHABLE_ROWS)} (-{len(CATCHABLE_REGRESSION_ROWS)} named "
            "regression-only entries). The table changed; update this script's "
            "corpus to match."
        )
    gate_failures += check_named_regression_rows(CATCHABLE_ROWS, CATCHABLE_REGRESSION_ROWS, "CATCHABLE_ROWS")

    spec_fatal_row_count = load_spec_fatal_row_count()
    if spec_fatal_row_count != len(FATAL_ROWS) - len(FATAL_REGRESSION_ROWS):
        gate_failures.append(
            f"check_fault_table.py: spec/04-semantics.md's Halting Faults "
            f"table has {spec_fatal_row_count} row(s), but FATAL_ROWS covers "
            f"{len(FATAL_ROWS)} (-{len(FATAL_REGRESSION_ROWS)} named regression-only "
            "entries). The table changed; update this script's corpus to match."
        )
    gate_failures += check_named_regression_rows(FATAL_ROWS, FATAL_REGRESSION_ROWS, "FATAL_ROWS")

    if gate_failures:
        for f in gate_failures:
            print(f, file=sys.stderr)
        sys.exit(2)

    rows = ALL_ROWS
    if args.only:
        rows = [r for r in rows if args.only in r.name]
        if not rows:
            print(f"check_fault_table.py: no row matches --only {args.only!r}", file=sys.stderr)
            sys.exit(2)

    diverged = 0
    skipped_rows = 0
    per_consumer_skips = {JVM: 0, BOOTSTRAP: 0}
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
                    f"program: {native_result.report!r} -- corpus bug, fix the row"
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
            for consumer in (JVM, BOOTSTRAP):
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
    print(f"rows checked: {len(rows) - skipped_rows}")
    print(f"rows not runnable: {skipped_rows}")
    print(f"divergences: {diverged}")
    print(
        "per-consumer row skips: "
        + ", ".join(f"{consumer}={count}" for consumer, count in per_consumer_skips.items())
    )
    sys.exit(1 if diverged else 0)


if __name__ == "__main__":
    main()
