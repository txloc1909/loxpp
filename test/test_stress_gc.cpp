// test_stress_gc.cpp — GC correctness under maximum allocation pressure.
//
// This binary is always compiled with LOXPP_STRESS_GC, which sets m_nextGC=0
// so every allocation (create<T> and rawAlloc) triggers a full collection.
// The tests verify that ordinary programs produce correct results even when
// every single allocation may free unreachable objects.

#include "test_harness.h"
#include <gtest/gtest.h>

class StressGCTest : public ::testing::Test {};

// ---------------------------------------------------------------------------
// String allocation and interning
// ---------------------------------------------------------------------------

TEST_F(StressGCTest, StringConcatSurvivesGC) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var a = "hello";
        var b = "world";
        var c = a + " " + b;
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("c"), "hello world");
    EXPECT_EQ(h.stackDepth(), 0);
}

// ---------------------------------------------------------------------------
// Global function declaration and call
// ---------------------------------------------------------------------------

TEST_F(StressGCTest, GlobalFunctionSurvivesGC) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun add(a, b) { return a + b; }
        var result = add(1, 2);
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("result");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(as<Number>(*v), 3.0);
    EXPECT_EQ(h.stackDepth(), 0);
}

// ---------------------------------------------------------------------------
// Local function declaration (the GC-unsafe path fixed by #32 — kept for
// documentation, not the focus of this PR, but stress mode surfaces it if
// present).
// ---------------------------------------------------------------------------

TEST_F(StressGCTest, LocalFunctionSurvivesGC) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun outer() {
            fun inner(x) { return x * 2; }
            return inner(21);
        }
        var result = outer();
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("result");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(as<Number>(*v), 42.0);
    EXPECT_EQ(h.stackDepth(), 0);
}

// ---------------------------------------------------------------------------
// Closure capture and mutation
// ---------------------------------------------------------------------------

TEST_F(StressGCTest, ClosureCaptureSurvivesGC) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun makeCounter() {
            var count = 0;
            fun increment() { count = count + 1; return count; }
            return increment;
        }
        var counter = makeCounter();
        counter();
        var result = counter();
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("result");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(as<Number>(*v), 2.0);
    EXPECT_EQ(h.stackDepth(), 0);
}

// ---------------------------------------------------------------------------
// Recursion (many stack frames + string/numeric allocations)
// ---------------------------------------------------------------------------

TEST_F(StressGCTest, RecursionSurvivesGC) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun fib(n) {
            if (n <= 1) return n;
            return fib(n - 1) + fib(n - 2);
        }
        var result = fib(10);
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("result");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(as<Number>(*v), 55.0);
    EXPECT_EQ(h.stackDepth(), 0);
}

// ---------------------------------------------------------------------------
// Native functions (defineNative allocates ObjNative then calls makeString)
// ---------------------------------------------------------------------------

TEST_F(StressGCTest, NativeFunctionSurvivesGC) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var s = str(42);
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("s"), "42");
    EXPECT_EQ(h.stackDepth(), 0);
}
