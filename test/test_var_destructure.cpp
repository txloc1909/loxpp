#include "test_harness.h"
#include <gtest/gtest.h>

static void expect_num(const Value& v, double expected) {
    ASSERT_TRUE(std::holds_alternative<Number>(v)) << "expected Number";
    EXPECT_NEAR(std::get<Number>(v), expected, 1e-9);
}

class VarDestructureTest : public ::testing::Test {};

// ===========================================================================
// Basic local-scope destructuring
// ===========================================================================

TEST_F(VarDestructureTest, TwoFieldsLocalScope) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Point { init(px, py) { this.x = px; this.y = py; } }
        fun test() {
            var obj = Point(10, 20);
            var {x, y} = obj;
            return x + y;
        }
        var result = test();
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("result");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 30);
}

TEST_F(VarDestructureTest, SingleFieldLocalScope) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Box {}
        fun test() {
            var p = Box();
            p.value = 99;
            var {value} = p;
            return value;
        }
        var result = test();
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("result");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 99);
}

TEST_F(VarDestructureTest, ThreeFieldsLocalScope) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Triple { init(a, b, c) { this.a = a; this.b = b; this.c = c; } }
        fun test() {
            var obj = Triple(1, 2, 3);
            var {a, b, c} = obj;
            return a + b + c;
        }
        var result = test();
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("result");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 6);
}

// ===========================================================================
// Multi-return function pattern (the primary motivating use case)
// ===========================================================================

TEST_F(VarDestructureTest, MultiReturnDivmod) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class DivResult { init(q, r) { this.quot = q; this.rem = r; } }
        fun divmod(a, b) { return DivResult(a / b, a % b); }
        fun test() {
            var {quot, rem} = divmod(17, 5);
            return rem;
        }
        var result = test();
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("result");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 2); // 17 % 5 == 2
}

// ===========================================================================
// Global-scope destructuring
// ===========================================================================

TEST_F(VarDestructureTest, GlobalScope) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Rect { init(w, h) { this.width = w; this.height = h; } }
        var g = Rect(5, 8);
        var {width, height} = g;
    )"),
              InterpretResult::OK);
    auto w = h.getGlobal("width");
    auto ht = h.getGlobal("height");
    ASSERT_TRUE(w.has_value());
    ASSERT_TRUE(ht.has_value());
    expect_num(*w, 5);
    expect_num(*ht, 8);
}

TEST_F(VarDestructureTest, GlobalScopeSingleField) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Wrapper { init(n) { this.n = n; } }
        var src = Wrapper(42);
        var {n} = src;
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("n");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 42);
}

// ===========================================================================
// Bound variables are mutable
// ===========================================================================

TEST_F(VarDestructureTest, BoundVarsAreMutable) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Box {}
        fun test() {
            var obj = Box();
            obj.a = 1;
            var {a} = obj;
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
// Destructuring from a call result directly (no intermediate named variable)
// ===========================================================================

TEST_F(VarDestructureTest, DestructureFromCallResult) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Pt { init(px, py) { this.x = px; this.y = py; } }
        fun make_point(px, py) { return Pt(px, py); }
        fun test() {
            var {x, y} = make_point(3, 4);
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
// Scope isolation — destructured locals don't leak out of function
// ===========================================================================

TEST_F(VarDestructureTest, LocalsDoNotLeakToGlobal) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Box {}
        fun test() {
            var obj = Box();
            obj.secret = 7;
            var {secret} = obj;
        }
        test();
    )"),
              InterpretResult::OK);
    EXPECT_FALSE(h.getGlobal("secret").has_value());
}

// ===========================================================================
// Stack stays neutral after destructuring exits scope
// ===========================================================================

TEST_F(VarDestructureTest, StackNeutralAfterDestructure) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Pair { init(a, b) { this.p = a; this.q = b; } }
        fun test() {
            var obj = Pair(1, 2);
            var {p, q} = obj;
        }
        test();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.stackDepth(), 0);
}
