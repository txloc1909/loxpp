// test_stress_gc.cpp — GC correctness under maximum allocation pressure.
//
// ctest runs this binary with LOXPP_STRESS_GC=1 in the environment (see the
// test/CMakeLists.txt registration), so MemoryManager triggers a full
// collection on every allocation (create<T> and rawAlloc). The tests verify
// that ordinary programs produce correct results even when every single
// allocation may free unreachable objects.

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

// The operands of a concatenation and the source of a slice are only on the
// stack. Both are longer than the inline buffer of a string, so building the
// result allocates, and that allocation collects: the operands must still be
// rooted then.
TEST_F(StressGCTest, LongStringConcatAndSliceSurviveGC) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var a = "abcdefghijklmnopqrstuvwxyz";
        var b = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        var c = (a + a) + (b + b);
        var s = (a + b + a)[3:49];
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("c"),
              "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
              "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZ");
    EXPECT_EQ(h.getGlobalStr("s"),
              "defghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVW");
    EXPECT_EQ(h.stackDepth(), 0);
}

// A String keeps the capacity of its bytes for life, so a concatenation and a
// slice make them with the exact size. 20 and 18 bytes are in the range where
// reserve() rounds the capacity up in libstdc++.
TEST_F(StressGCTest, ConcatAndSliceKeepAnExactCapacity) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var a = "abcdefghij";
        var c = a + "klmnopqrst";
        var s = c[1:19];
    )"),
              InterpretResult::OK);
    auto c = h.getGlobal("c");
    auto s = h.getGlobal("s");
    ASSERT_TRUE(c.has_value() && s.has_value());
    const auto& cChars = asObjString(as<Obj*>(*c))->chars;
    const auto& sChars = asObjString(as<Obj*>(*s))->chars;
    EXPECT_EQ(cChars.size(), 20u);
    EXPECT_EQ(cChars.capacity(), 20u);
    EXPECT_EQ(sChars.size(), 18u);
    EXPECT_EQ(sChars.capacity(), 18u);
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
