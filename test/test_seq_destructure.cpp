#include "test_harness.h"
#include <gtest/gtest.h>

static void expect_num(const Value& v, double expected) {
    ASSERT_TRUE(is<Number>(v)) << "expected Number";
    EXPECT_NEAR(as<Number>(v), expected, 1e-9);
}

static void expect_str(const Value& v, const std::string& expected) {
    ASSERT_TRUE(isString(v)) << "expected String";
    EXPECT_EQ(asObjString(as<Obj*>(v))->chars.c_str(), expected);
}

class SeqDestructureTest : public ::testing::Test {};

// ===========================================================================
// Basic local-scope destructuring
// ===========================================================================

TEST_F(SeqDestructureTest, TwoElementsLocalScope) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun test() {
            var [a, b] = [10, 20];
            return a + b;
        }
        var result = test();
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("result");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 30);
}

TEST_F(SeqDestructureTest, SingleElementLocalScope) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun test() {
            var [x] = [42];
            return x;
        }
        var result = test();
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("result");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 42);
}

TEST_F(SeqDestructureTest, ThreeElementsLocalScope) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun test() {
            var [x, y, z] = [1, 2, 3];
            return x + y + z;
        }
        var result = test();
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("result");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 6);
}

// ===========================================================================
// Global-scope destructuring
// ===========================================================================

TEST_F(SeqDestructureTest, GlobalScope) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var [a, b] = [5, 8];
    )"),
              InterpretResult::OK);
    auto av = h.getGlobal("a");
    auto bv = h.getGlobal("b");
    ASSERT_TRUE(av.has_value());
    ASSERT_TRUE(bv.has_value());
    expect_num(*av, 5);
    expect_num(*bv, 8);
}

// ===========================================================================
// Destructuring from a function return value
// ===========================================================================

TEST_F(SeqDestructureTest, DestructureFromCallResult) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun make_pair(a, b) { return [a, b]; }
        fun test() {
            var [x, y] = make_pair(3, 4);
            return x * x + y * y;
        }
        var result = test();
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("result");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 25);
}

// ===========================================================================
// Wildcard _ — positional, no binding
// ===========================================================================

TEST_F(SeqDestructureTest, WildcardInMiddle) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun test() {
            var [a, _, b] = [1, 2, 3];
            return a + b;
        }
        var result = test();
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("result");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 4);
}

TEST_F(SeqDestructureTest, WildcardAtEndLongerList) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun test() {
            var [a, b, _] = [10, 20, 30, 40];
            return a + b;
        }
        var result = test();
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("result");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 30);
}

TEST_F(SeqDestructureTest, WildcardDoesNotCreateBinding) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun test() {
            var [a, _] = [1, 2];
            return a;
        }
        test();
    )"),
              InterpretResult::OK);
    EXPECT_FALSE(h.getGlobal("_").has_value());
}

// ===========================================================================
// String character unpacking
// ===========================================================================

TEST_F(SeqDestructureTest, StringCharUnpack) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun test() {
            var [first, second] = "hi";
            return first;
        }
        var result = test();
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("result");
    ASSERT_TRUE(v.has_value());
    expect_str(*v, "h");
}

// ===========================================================================
// Trailing comma is valid
// ===========================================================================

TEST_F(SeqDestructureTest, TrailingCommaIsValid) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun test() {
            var [a,] = [7];
            return a;
        }
        var result = test();
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("result");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 7);
}

// ===========================================================================
// Bound variables are mutable
// ===========================================================================

TEST_F(SeqDestructureTest, BoundVarsAreMutable) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun test() {
            var [a] = [1];
            a = 42;
            return a;
        }
        var result = test();
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("result");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 42);
}

// ===========================================================================
// Stack stays neutral after destructuring exits scope
// ===========================================================================

TEST_F(SeqDestructureTest, StackNeutralAfterDestructure) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun test() {
            var [p, q] = [1, 2];
        }
        test();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.stackDepth(), 0);
}

// ===========================================================================
// Locals do not leak out of function scope
// ===========================================================================

TEST_F(SeqDestructureTest, LocalsDoNotLeakToGlobal) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun test() {
            var [secret] = [7];
        }
        test();
    )"),
              InterpretResult::OK);
    EXPECT_FALSE(h.getGlobal("secret").has_value());
}

// ===========================================================================
// Out-of-bounds raises a runtime error
// ===========================================================================

TEST_F(SeqDestructureTest, OutOfBoundsRuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run(R"(
        fun test() {
            var [a, b] = [1];
            return a + b;
        }
        test();
    )"),
              InterpretResult::RUNTIME_ERROR);
}
