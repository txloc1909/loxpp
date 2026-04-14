// test_list.cpp — Tests for the List type and [] indexing.

#include "test_harness.h"
#include <gtest/gtest.h>

// ---------------------------------------------------------------------------
// List literals
// ---------------------------------------------------------------------------

TEST(List, EmptyLiteral) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = []; print xs;"), InterpretResult::OK);
}

TEST(List, SingleElement) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [42];"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("xs"), "[42]");
}

TEST(List, HeterogeneousLiteral) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var xs = [1, "hello", true, nil, false];)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("xs"), "[1, hello, true, nil, false]");
}

// ---------------------------------------------------------------------------
// Index get
// ---------------------------------------------------------------------------

TEST(List, IndexGetFirst) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [10, 20, 30]; var r = xs[0];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "10");
}

TEST(List, IndexGetLast) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [10, 20, 30]; var r = xs[2];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "30");
}

TEST(List, IndexGetExpression) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [10, 20, 30]; var i = 1; var r = xs[i + 1];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "30");
}

TEST(List, IndexGetIntegerValuedFloat) {
    // 1.0 is the same value as 1 — must be accepted
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [10, 20, 30]; var r = xs[1.0];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "20");
}

// ---------------------------------------------------------------------------
// Index set
// ---------------------------------------------------------------------------

TEST(List, IndexSet) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [1, 2, 3]; xs[1] = 99; var r = xs[1];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "99");
}

TEST(List, IndexSetIsExpression) {
    // Assignment expression evaluates to the assigned value
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [0]; var r = xs[0] = 42;"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "42");
}

TEST(List, IndexSetExpressionIndex) {
    // Bug: compiling the index expression (e.g. j + 1) clobbered m_canAssign,
    // so xs[j + 1] = v was miscompiled as GET_INDEX followed by a stray '='.
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var xs = [10, 20, 30];
        var j = 0;
        xs[j + 1] = 99;
        var r = xs[1];
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "99");
}

TEST(List, IndexSetExpressionIndexInsideFunction) {
    // Same as IndexSetExpressionIndex but inside a function scope (exercises
    // local-variable index paths in the compiler).
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun setAt(xs, j, v) { xs[j + 1] = v; }
        var xs = [10, 20, 30];
        setAt(xs, 0, 99);
        var r = xs[1];
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "99");
}

TEST(List, IndexSetSwapPattern) {
    // In-place swap via expression indices — the core of bubble / insertion
    // sort.
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var xs = [1, 2, 3];
        var j = 0;
        var t = xs[j];
        xs[j]     = xs[j + 1];
        xs[j + 1] = t;
        var r0 = xs[0];
        var r1 = xs[1];
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r0"), "2");
    EXPECT_EQ(h.getGlobalStr("r1"), "1");
}

TEST(List, IndexSetAllElements) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var xs = [0, 0, 0];
        xs[0] = 10;
        xs[1] = 20;
        xs[2] = 30;
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("xs"), "[10, 20, 30]");
}

// ---------------------------------------------------------------------------
// Chaining
// ---------------------------------------------------------------------------

TEST(List, NestedListIndexing) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var m = [[1, 2], [3, 4]]; var r = m[1][0];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "3");
}

TEST(List, IndexThenCallElement) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun double(x) { return x * 2; }
        var fns = [double];
        var r = fns[0](5);
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "10");
}

TEST(List, IndexResultFromCall) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun makeList() { return [7, 8, 9]; }
        var r = makeList()[2];
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "9");
}

// ---------------------------------------------------------------------------
// Lists as first-class values
// ---------------------------------------------------------------------------

TEST(List, PassedAsArgument) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun first(xs) { return xs[0]; }
        var r = first([42, 43]);
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "42");
}

TEST(List, StoredInClosure) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var xs = [1, 2, 3];
        fun getSecond() { return xs[1]; }
        xs[1] = 99;
        var r = getSecond();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "99");
}

TEST(List, ListIdentityEquality) {
    // Two distinct list literals are not equal even if they have the same
    // elements
    VMTestHarness h;
    ASSERT_EQ(h.run("var a = [1, 2]; var b = [1, 2]; var r = a == b;"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "false");
}

TEST(List, SameListIsEqual) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var a = [1, 2]; var b = a; var r = a == b;"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "true");
}

// ---------------------------------------------------------------------------
// Stringify / print
// ---------------------------------------------------------------------------

TEST(List, StringifyNested) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [[1, 2], [3, 4]];"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("xs"), "[[1, 2], [3, 4]]");
}

TEST(List, StringifyMixed) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var xs = [1, "hi", true, nil];)"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("xs"), "[1, hi, true, nil]");
}

// ---------------------------------------------------------------------------
// Runtime errors
// ---------------------------------------------------------------------------

TEST(List, NonNumberIndex) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var xs = [1, 2]; xs["a"];)"),
              InterpretResult::RUNTIME_ERROR);
}

TEST(List, FractionalIndex) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [1, 2, 3]; xs[1.5];"),
              InterpretResult::RUNTIME_ERROR);
}

TEST(List, NegativeIndex) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [1, 2, 3]; xs[-1];"),
              InterpretResult::RUNTIME_ERROR);
}

TEST(List, IndexOutOfBounds) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [1, 2, 3]; xs[3];"),
              InterpretResult::RUNTIME_ERROR);
}

TEST(List, IndexNonList) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = 42; x[0];"), InterpretResult::RUNTIME_ERROR);
}

TEST(List, SetIndexOutOfBounds) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [1, 2]; xs[5] = 99;"),
              InterpretResult::RUNTIME_ERROR);
}

TEST(List, BooleanIndex) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [1, 2]; xs[true];"),
              InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// GC stress: list survives collection
// ---------------------------------------------------------------------------

TEST(List, SurvivesGC) {
    VMTestHarness h;
    // Allocate many strings to provoke GC; list must remain intact.
    ASSERT_EQ(h.run(R"(
        var xs = [1, 2, 3];
        var i = 0;
        while (i < 50) {
            var s = str(i);
            i = i + 1;
        }
        var r = xs[2];
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "3");
}

TEST(List, ElementsSurviveGC) {
    VMTestHarness h;
    // Elements that are strings must be kept alive by the list.
    ASSERT_EQ(h.run(R"(
        var xs = ["keep-me", "also-me"];
        var i = 0;
        while (i < 50) {
            var s = str(i);
            i = i + 1;
        }
        var r = xs[0];
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "keep-me");
}

// ---------------------------------------------------------------------------
// append
// ---------------------------------------------------------------------------

TEST(List, AppendToEmpty) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = []; xs.append(1); var r = xs[0];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "1");
}

TEST(List, AppendMultiple) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [1, 2]; xs.append(3); xs.append(4);"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("xs"), "[1, 2, 3, 4]");
}

TEST(List, AppendReturnsNil) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = []; var r = xs.append(99);"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "nil");
}

TEST(List, AppendMaintainsOrder) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var xs = [];
        xs.append(10);
        xs.append(20);
        xs.append(30);
        var r = xs[2];
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "30");
}

TEST(List, AppendWrongArgCount) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = []; xs.append();"),
              InterpretResult::RUNTIME_ERROR);
}

TEST(List, AppendTooManyArgs) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = []; xs.append(1, 2);"),
              InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// pop
// ---------------------------------------------------------------------------

TEST(List, PopReturnsLastElement) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [1, 2, 3]; var r = xs.pop();"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "3");
}

TEST(List, PopShrinksListByOne) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [1, 2, 3]; xs.pop();"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("xs"), "[1, 2]");
}

TEST(List, PopUntilEmpty) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var xs = [10, 20];
        var a = xs.pop();
        var b = xs.pop();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("a"), "20");
    EXPECT_EQ(h.getGlobalStr("b"), "10");
    EXPECT_EQ(h.getGlobalStr("xs"), "[]");
}

TEST(List, PopEmptyList) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = []; xs.pop();"), InterpretResult::RUNTIME_ERROR);
}

TEST(List, PopWrongArgCount) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [1]; xs.pop(1);"),
              InterpretResult::RUNTIME_ERROR);
}

TEST(List, AppendThenPop) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var xs = [1, 2];
        xs.append(99);
        var r = xs.pop();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "99");
    EXPECT_EQ(h.getGlobalStr("xs"), "[1, 2]");
}

// ---------------------------------------------------------------------------
// len
// ---------------------------------------------------------------------------

TEST(List, LenEmpty) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var r = len([]);"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "0");
}

TEST(List, LenNonEmpty) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var r = len([1, 2, 3]);"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "3");
}

TEST(List, LenAfterAppend) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var xs = [1, 2];
        xs.append(3);
        var r = len(xs);
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "3");
}

TEST(List, LenAfterPop) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var xs = [1, 2, 3];
        xs.pop();
        var r = len(xs);
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "2");
}

TEST(List, LenUsedAsBound) {
    // Common pattern: iterate with len as loop bound
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var xs = [10, 20, 30];
        var sum = 0;
        var i = 0;
        while (i < len(xs)) {
            sum = sum + xs[i];
            i = i + 1;
        }
        var r = sum;
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "60");
}

TEST(List, LenOnNonList) {
    VMTestHarness h;
    ASSERT_EQ(h.run("len(42);"), InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// Undefined method
// ---------------------------------------------------------------------------

TEST(List, UndefinedMethod) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [1, 2]; xs.foo();"),
              InterpretResult::RUNTIME_ERROR);
}
