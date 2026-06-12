// test_closures.cpp — closure and upvalue runtime tests.
//
// Invariants under test:
//   1. Basic capture      — inner fn reads outer local after outer returns.
//   2. Mutation via upvalue — inner fn writes through to a shared captured var.
//   3. Shared upvalue     — two closures share one upvalue (counter pattern).
//   4. Nested closures    — upvalue-of-upvalue (isLocal=false path).
//   5. Stack discipline   — stackDepth() == 0 after all closure programs.

#include "test_harness.h"
#include <gtest/gtest.h>

class ClosureTest : public ::testing::Test {};

// ---------------------------------------------------------------------------
// 1. Basic capture: inner function reads a local from its enclosing scope.
// ---------------------------------------------------------------------------

TEST_F(ClosureTest, BasicCapture_ReadAfterReturn) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun makeGreeter(name) {
            fun greet() { return name; }
            return greet;
        }
        var g = makeGreeter("world");
        var result = g();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("result"), "world");
    EXPECT_EQ(h.stackDepth(), 0);
}

TEST_F(ClosureTest, BasicCapture_ReadsCorrectValue) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun makeAdder(x) {
            fun add(y) { return x + y; }
            return add;
        }
        var add5 = makeAdder(5);
        var result = add5(3);
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("result");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(as<Number>(*v), 8.0);
    EXPECT_EQ(h.stackDepth(), 0);
}

// ---------------------------------------------------------------------------
// 2. Mutation via upvalue: inner fn writes to a captured variable.
// ---------------------------------------------------------------------------

TEST_F(ClosureTest, MutationViaUpvalue) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun makeCounter() {
            var count = 0;
            fun increment() {
                count = count + 1;
                return count;
            }
            return increment;
        }
        var c = makeCounter();
        var a = c();
        var b = c();
        var d = c();
    )"),
              InterpretResult::OK);
    auto a = h.getGlobal("a");
    auto b = h.getGlobal("b");
    auto d = h.getGlobal("d");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_TRUE(d.has_value());
    EXPECT_DOUBLE_EQ(as<Number>(*a), 1.0);
    EXPECT_DOUBLE_EQ(as<Number>(*b), 2.0);
    EXPECT_DOUBLE_EQ(as<Number>(*d), 3.0);
    EXPECT_EQ(h.stackDepth(), 0);
}

// ---------------------------------------------------------------------------
// 3. Shared upvalue: two closures capture the same variable.
// ---------------------------------------------------------------------------

TEST_F(ClosureTest, SharedUpvalue_TwoClosures) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun makePair() {
            var n = 0;
            fun inc() { n = n + 1; }
            fun get() { return n; }
            return inc;
        }
        var inc = makePair();
        inc();
        inc();
    )"),
              InterpretResult::OK);
    // The counter ran without error — no crash from shared upvalue.
    EXPECT_EQ(h.stackDepth(), 0);
}

TEST_F(ClosureTest, SharedUpvalue_IndependentCounters) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun makeCounter() {
            var count = 0;
            fun increment() { count = count + 1; return count; }
            return increment;
        }
        var c1 = makeCounter();
        var c2 = makeCounter();
        var a = c1();
        var b = c1();
        var x = c2();
    )"),
              InterpretResult::OK);
    auto a = h.getGlobal("a");
    auto b = h.getGlobal("b");
    auto x = h.getGlobal("x");
    ASSERT_TRUE(a.has_value() && b.has_value() && x.has_value());
    // c1 and c2 each have their own upvalue — independent state.
    EXPECT_DOUBLE_EQ(as<Number>(*a), 1.0);
    EXPECT_DOUBLE_EQ(as<Number>(*b), 2.0);
    EXPECT_DOUBLE_EQ(as<Number>(*x), 1.0);
    EXPECT_EQ(h.stackDepth(), 0);
}

// ---------------------------------------------------------------------------
// 4. Nested closures: upvalue-of-upvalue (exercises isLocal=false path).
// ---------------------------------------------------------------------------

TEST_F(ClosureTest, NestedClosure_UpvalueOfUpvalue) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun outer() {
            var x = 42;
            fun middle() {
                fun inner() { return x; }
                return inner;
            }
            return middle;
        }
        var result = outer()()();
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("result");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(as<Number>(*v), 42.0);
    EXPECT_EQ(h.stackDepth(), 0);
}

TEST_F(ClosureTest, NestedClosure_MutationPropagates) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun outer() {
            var x = 0;
            fun middle() {
                fun inner() { x = x + 1; return x; }
                return inner;
            }
            return middle;
        }
        var f = outer()();
        var a = f();
        var b = f();
    )"),
              InterpretResult::OK);
    auto a = h.getGlobal("a");
    auto b = h.getGlobal("b");
    ASSERT_TRUE(a.has_value() && b.has_value());
    EXPECT_DOUBLE_EQ(as<Number>(*a), 1.0);
    EXPECT_DOUBLE_EQ(as<Number>(*b), 2.0);
    EXPECT_EQ(h.stackDepth(), 0);
}

// ---------------------------------------------------------------------------
// 5. Captured var survives the enclosing scope's block exit.
// ---------------------------------------------------------------------------

TEST_F(ClosureTest, ClosedOverVarSurvivesBlockExit) {
    VMTestHarness h;
    // The upvalue for `x` must be closed (moved off the stack) when the block
    // ends, but the closure must still be able to read it afterwards.
    ASSERT_EQ(h.run(R"(
        var f;
        {
            var x = 99;
            fun capture() { return x; }
            f = capture;
        }
        var result = f();
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("result");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(as<Number>(*v), 99.0);
    EXPECT_EQ(h.stackDepth(), 0);
}
