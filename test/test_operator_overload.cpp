// test_operator_overload.cpp — Tests for operator overloading (issue #472),
// tier T1: arithmetic, comparison, negation, containment, call, and equality
// dispatch to dunder methods on an Instance.

#include "test_harness.h"
#include <gtest/gtest.h>

// ---------------------------------------------------------------------------
// Arithmetic dispatch
// ---------------------------------------------------------------------------

TEST(OperatorOverload, ArithmeticDispatch) {
    VMTestHarness h;
    std::string src = "class V { init(x) { this.x = x; }"
                      "  __add__(o) { return this.x + o.x; }"
                      "  __sub__(o) { return this.x - o.x; }"
                      "  __mul__(o) { return this.x * o.x; }"
                      "  __div__(o) { return this.x / o.x; }"
                      "  __mod__(o) { return this.x % o.x; }"
                      "  __neg__() { return -this.x; }"
                      "}"
                      "var add = V(3) + V(4);"
                      "var sub = V(3) - V(4);"
                      "var mul = V(3) * V(4);"
                      "var div = V(3) / V(4);"
                      "var mod = V(3) % V(4);"
                      "var neg = -V(3);";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("add"), "7");
    EXPECT_EQ(h.getGlobalStr("sub"), "-1");
    EXPECT_EQ(h.getGlobalStr("mul"), "12");
    EXPECT_EQ(h.getGlobalStr("div"), "0.75");
    EXPECT_EQ(h.getGlobalStr("mod"), "3");
    EXPECT_EQ(h.getGlobalStr("neg"), "-3");
}

// The built-in fast path still wins for Numbers and Strings: a program that
// never defines a method sees unchanged behaviour.
TEST(OperatorOverload, BuiltinFastPathUnchanged) {
    VMTestHarness h;
    std::string src = "var a = 1 + 2;"
                      "var b = 3 * 4;"
                      "var c = \"x\" + \"y\";"
                      "var d = 7 % 3;"
                      "var e = -3 % 2;";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("a"), "3");
    EXPECT_EQ(h.getGlobalStr("b"), "12");
    EXPECT_EQ(h.getGlobalStr("c"), "xy");
    EXPECT_EQ(h.getGlobalStr("d"), "1");
    EXPECT_EQ(h.getGlobalStr("e"), "1"); // floor-division: -3 % 2 == 1
}

// ---------------------------------------------------------------------------
// Comparison and equality dispatch
// ---------------------------------------------------------------------------

TEST(OperatorOverload, ComparisonAndEqualityDispatch) {
    VMTestHarness h;
    std::string src = "class V { init(x) { this.x = x; }"
                      "  __lt__(o) { return this.x < o.x; }"
                      "  __gt__(o) { return this.x > o.x; }"
                      "  __eq__(o) { return this.x == o.x; }"
                      "}"
                      "var a = V(3); var b = V(4);"
                      "var lt = a < b;"
                      "var gt = a > b;"
                      "var eq = a == b;"
                      "var ne = a != b;"
                      "var le = a <= b;"
                      "var ge = a >= b;";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("lt"), "true");
    EXPECT_EQ(h.getGlobalStr("gt"), "false");
    EXPECT_EQ(h.getGlobalStr("eq"), "false");
    EXPECT_EQ(h.getGlobalStr("ne"), "true");  // derives from __eq__ then NOT
    EXPECT_EQ(h.getGlobalStr("le"), "true");  // !(a > b)
    EXPECT_EQ(h.getGlobalStr("ge"), "false"); // !(a < b)
}

// ---------------------------------------------------------------------------
// Containment and call dispatch
// ---------------------------------------------------------------------------

TEST(OperatorOverload, ContainsAndCallDispatch) {
    VMTestHarness h;
    std::string src = "class Box { init(x) { this.x = x; }"
                      "  __contains__(v) { return v == this.x; }"
                      "  __call__(n) { return this.x + n; }"
                      "}"
                      "var box = Box(10);"
                      "var yes = 10 in box;"
                      "var no = 3 in box;"
                      "var call = box(5);";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("yes"), "true");
    EXPECT_EQ(h.getGlobalStr("no"), "false");
    EXPECT_EQ(h.getGlobalStr("call"), "15");
}

// ---------------------------------------------------------------------------
// Result validation (OperatorResultTypeError)
// ---------------------------------------------------------------------------

TEST(OperatorOverload, NonBooleanEqResultRaises) {
    VMTestHarness h;
    std::string src =
        "class V { __eq__(o) { return 42; } }"
        "var kind;"
        "try { var r = V() == V(); } catch (e) { kind = e.kind; }";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("kind"), "OperatorResultTypeError");
}

TEST(OperatorOverload, NonBooleanComparisonResultRaises) {
    VMTestHarness h;
    std::string src =
        "class V { __lt__(o) { return 1; } __gt__(o) { return 2; } }"
        "var kind1; var kind2;"
        "try { var r = V() < V(); } catch (e) { kind1 = e.kind; }"
        "try { var r = V() > V(); } catch (e) { kind2 = e.kind; }";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("kind1"), "OperatorResultTypeError");
    EXPECT_EQ(h.getGlobalStr("kind2"), "OperatorResultTypeError");
}

TEST(OperatorOverload, NonBooleanContainsResultRaises) {
    VMTestHarness h;
    std::string src = "class V { __contains__(o) { return \"nope\"; } }"
                      "var kind;"
                      "try { var r = 1 in V(); } catch (e) { kind = e.kind; }";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("kind"), "OperatorResultTypeError");
}

// ---------------------------------------------------------------------------
// Missing method falls back to the same error as today
// ---------------------------------------------------------------------------

TEST(OperatorOverload, MissingMethodRaisesSameError) {
    VMTestHarness h;
    std::string src = "class V {}"
                      "var kind1; var kind2; var kind3; var kind4;"
                      "try { var r = V() + 1; } catch (e) { kind1 = e.kind; }"
                      "try { var r = V() < 1; } catch (e) { kind2 = e.kind; }"
                      "try { var r = -V(); } catch (e) { kind3 = e.kind; }"
                      "try { var r = V()(); } catch (e) { kind4 = e.kind; }";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("kind1"), "ConcatenationTypeError");
    EXPECT_EQ(h.getGlobalStr("kind2"), "ComparisonTypeError");
    EXPECT_EQ(h.getGlobalStr("kind3"), "ArithmeticTypeError");
    EXPECT_EQ(h.getGlobalStr("kind4"), "NotCallableError");
}

// ---------------------------------------------------------------------------
// Lookup reads the method table only
// ---------------------------------------------------------------------------

TEST(OperatorOverload, FieldNamedLikeMethodDoesNotDispatch) {
    VMTestHarness h;
    std::string src = "class V { init() { this.__add__ = 99; } }"
                      "var kind;"
                      "try { var r = V() + 1; } catch (e) { kind = e.kind; }";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("kind"), "ConcatenationTypeError");
}

// ---------------------------------------------------------------------------
// Internal equality keeps identity semantics
// ---------------------------------------------------------------------------

TEST(OperatorOverload, ListMembershipUsesIdentityNotEq) {
    VMTestHarness h;
    std::string src = "class V { __eq__(o) { return true; } }"
                      "var a = V(); var b = V();"
                      "var member = a in [b];"
                      "var eq = a == b;";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("member"), "false"); // identity, not __eq__
    EXPECT_EQ(h.getGlobalStr("eq"), "true");      // __eq__ applies to `==`
}

// A `__call__` method that throws propagates the throw, rather than being
// mistaken for a non-Boolean result (the result-validation path is only for
// the boolean-validated operators).
TEST(OperatorOverload, CallMethodThrowPropagates) {
    VMTestHarness h;
    std::string src = "class V { __call__() { throw \"boom\"; } }"
                      "var caught;"
                      "try { V()(); } catch (e) { caught = e; }";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("caught"), "boom");
}

// ---------------------------------------------------------------------------
// Index get/set dispatch (__index_get__ / __index_set__)
// ---------------------------------------------------------------------------

TEST(OperatorOverload, IndexGetDispatch) {
    VMTestHarness h;
    std::string src = "class V { init() { this.x = 5; }"
                      "  __index_get__(k) { return this.x + k; }"
                      "}"
                      "var v = V();"
                      "var r = v[10];";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "15");
}

// The assignment expression evaluates to the assigned value, not the
// method's return.
TEST(OperatorOverload, IndexSetDispatchAssignmentValue) {
    VMTestHarness h;
    std::string src = "class V { init() { this.x = 0; }"
                      "  __index_get__(k) { return this.x; }"
                      "  __index_set__(k, v) { this.x = v; return 12345; }"
                      "}"
                      "var v = V();"
                      "var s = (v[\"a\"] = 7);"
                      "var x = v[0];";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("s"), "7"); // assigned value, not 12345
    EXPECT_EQ(h.getGlobalStr("x"), "7"); // __index_set__ stored 7
}
// A missing __index_get__/__index_set__ raises the same error as today.
TEST(OperatorOverload, IndexMissingMethodRaisesSameError) {
    VMTestHarness h;
    std::string src = "class V {}"
                      "var kind1; var kind2;"
                      "try { var r = V()[0]; } catch (e) { kind1 = e.kind; }"
                      "try { V()[0] = 1; } catch (e) { kind2 = e.kind; }";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("kind1"), "NotIndexableError");
    EXPECT_EQ(h.getGlobalStr("kind2"), "NotIndexableError");
}

// ---------------------------------------------------------------------------
// len dispatch (__len__)
// ---------------------------------------------------------------------------

TEST(OperatorOverload, LenBuiltinUnchanged) {
    VMTestHarness h;
    std::string src = "var a = len([1, 2, 3]);"
                      "var b = len(\"ab\");"
                      "var c = len({1: 2, 3: 4});";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("a"), "3");
    EXPECT_EQ(h.getGlobalStr("b"), "2");
    EXPECT_EQ(h.getGlobalStr("c"), "2");
}

TEST(OperatorOverload, LenDispatch) {
    VMTestHarness h;
    std::string src = "class V { __len__() { return 7; } }"
                      "var n = len(V());";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("n"), "7");
}

TEST(OperatorOverload, NonNumberLenResultRaises) {
    VMTestHarness h;
    std::string src = "class V { __len__() { return \"nope\"; } }"
                      "var kind;"
                      "try { var n = len(V()); } catch (e) { kind = e.kind; }";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("kind"), "OperatorResultTypeError");
}

TEST(OperatorOverload, LenMissingMethodRaisesSameError) {
    VMTestHarness h;
    // The legacy `len` error is fatal, not catchable.
    ASSERT_EQ(h.run("class V {} var x = len(V());"),
              InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// for-in dispatch (__iter__)
// ---------------------------------------------------------------------------

TEST(OperatorOverload, IterDispatch) {
    VMTestHarness h;
    std::string src =
        "class R { init(n) { this.n = n; }"
        "  __iter__() { var o = []; var i = 0;"
        "               while (i < this.n) { o.append(i); i = i + 1; }"
        "               return o; }"
        "}"
        "var sum = 0;"
        "for (var x in R(4)) { sum = sum + x; }";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("sum"), "6");
}

TEST(OperatorOverload, NonSequenceIterResultRaises) {
    VMTestHarness h;
    std::string src =
        "class V { __iter__() { return 42; } }"
        "var kind;"
        "try { for (var x in V()) {} } catch (e) { kind = e.kind; }";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("kind"), "OperatorResultTypeError");
}

TEST(OperatorOverload, IterMissingMethodRaisesSameError) {
    VMTestHarness h;
    // The legacy "not iterable" error is fatal, not catchable.
    ASSERT_EQ(h.run("class V {} for (var x in V()) {}"),
              InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// slice dispatch (__slice__)
// ---------------------------------------------------------------------------

TEST(OperatorOverload, SliceDispatch) {
    VMTestHarness h;
    // The result depends on the argument order, so a swapped start/end is
    // visible: 3*100 + 4 is not 4*100 + 3.
    std::string src = "class V { __slice__(s, e) { return s * 100 + e; } }"
                      "var v = V();"
                      "var r = v[3:4];";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "304");
}

// The built-in fast path still wins for List and String slices.
TEST(OperatorOverload, SliceBuiltinUnchanged) {
    VMTestHarness h;
    std::string src = "var l = [10, 20, 30, 40][1:3];"
                      "var s = \"hello\"[1:3];";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("l"), "[20, 30]");
    EXPECT_EQ(h.getGlobalStr("s"), "el");
}

TEST(OperatorOverload, SliceMissingMethodRaisesSameError) {
    VMTestHarness h;
    // The legacy slice error is fatal: a live catch does not stop it, so the
    // run reports a runtime error and never reaches the next statement.
    ASSERT_EQ(h.run("class V {}"
                    "try { V()[0:1]; } catch (e) { var caught = 1; }"
                    "var done = 1;"),
              InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// str dispatch (__str__)
// ---------------------------------------------------------------------------

TEST(OperatorOverload, StrDispatch) {
    VMTestHarness h;
    std::string src = "class V { __str__() { return \"custom\"; } }"
                      "class W {}"
                      "var a = str(V());"
                      "var b = str(W());";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("a"), "custom");
    EXPECT_EQ(h.getGlobalStr("b"), "W instance");
}

TEST(OperatorOverload, StrDispatchNested) {
    VMTestHarness h;
    std::string src = "class V { __str__() { return \"v\"; } }"
                      "var l = str([V(), 1]);"
                      "var m = str({\"k\": V()});";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("l"), "[v, 1]");
    EXPECT_EQ(h.getGlobalStr("m"), "{k: v}");
}

TEST(OperatorOverload, NonStringStrResultRaises) {
    VMTestHarness h;
    std::string src = "class V { __str__() { return 42; } }"
                      "var kind;"
                      "try { var s = str(V()); } catch (e) { kind = e.kind; }";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("kind"), "OperatorResultTypeError");
}

TEST(OperatorOverload, PrintStrResult) {
    VMTestHarness h;
    // print routes through the same canonical stringify as str(); a thrown
    // __str__ is catchable at the print site.
    std::string src = "class V { __str__() { return 42; } }"
                      "var kind;"
                      "try { print V(); } catch (e) { kind = e.kind; }";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("kind"), "OperatorResultTypeError");
}

TEST(OperatorOverload, RecursiveStrHitsDepthGuard) {
    VMTestHarness h;
    // A __str__ that calls str() on itself recurses until the canonical-string
    // depth limit fires a fatal fault — not a C++ crash.
    std::string src = "class V { __str__() { return str(this); } }"
                      "print V();";
    ASSERT_EQ(h.run(src), InterpretResult::RUNTIME_ERROR);
}

// A __str__ that composes another str()/print whose __str__ throws a caught
// error must not corrupt the outer stringify (the shared status must not leak
// from the nested, caught dispatch into the outer one).
TEST(OperatorOverload, NestedCaughtStrThrowDoesNotCorruptOuter) {
    VMTestHarness h;
    std::string src = "class W { __str__() { return 42; } }"
                      "class V { __str__() { try { str(W()); } catch (e) {}"
                      "                          return \"ok\"; } }"
                      "var a = str(V());";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("a"), "ok");
}

// An uncaught throw of a non-Error Instance reports its canonical __str__ form,
// matching the JVM and spec/04-semantics.md's "canonical string
// representation".
TEST(OperatorOverload, ThrownInstanceReportsStrResult) {
    VMTestHarness h;
    testing::internal::CaptureStderr();
    InterpretResult r =
        h.run("class V { __str__() { return \"CUSTOM\"; } } throw V();");
    std::string err = testing::internal::GetCapturedStderr();
    ASSERT_EQ(r, InterpretResult::RUNTIME_ERROR);
    EXPECT_NE(err.find("CUSTOM"), std::string::npos);
}
