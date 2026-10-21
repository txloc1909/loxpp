#include "test_harness.h"
#include "value.h"
#include "container_objects.h"

#include <gtest/gtest.h>

static void expect_num(Value v, double n) {
    ASSERT_TRUE(is<Number>(v));
    EXPECT_DOUBLE_EQ(as<Number>(v), n);
}

class AtBindingTest : public ::testing::Test {};

// ---------------------------------------------------------------------------
// Basic constructor pattern
// ---------------------------------------------------------------------------

// @-name binds the whole enum value; field binding still works.
TEST_F(AtBindingTest, ConstructorPositional) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Result { Ok(value) Err(msg) }
        var subject = Result.Ok(42);
        var field_val = match subject {
            case n @ Result.Ok(v) => v
            case _         => 0
        };
        // Re-match on the bound @-name to verify it is the whole Ok value.
        var at_val = match subject {
            case n @ Result.Ok(v) => match n {
                case Result.Ok(v2) => v2
                case _      => -1
            }
            case _ => -1
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("field_val"), 42);
    expect_num(*h.getGlobal("at_val"), 42);
}

// ---------------------------------------------------------------------------
// Named-field class pattern
// ---------------------------------------------------------------------------

TEST_F(AtBindingTest, ClassNamedField) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Point { init(x, y) { this.x = x; this.y = y; } }
        var pt = Point(3, 4);
        var sum = match pt {
            case p @ Point{x, y} => x + y
            case _               => 0
        };
        // p should be the original Point instance; access its field directly.
        var px = match pt {
            case p @ Point{x, y} => p.x
            case _               => -1
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("sum"), 7);
    expect_num(*h.getGlobal("px"), 3);
}

// ---------------------------------------------------------------------------
// Sequence pattern
// ---------------------------------------------------------------------------

TEST_F(AtBindingTest, SequencePattern) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var lst = [10, 20, 30];
        var head = match lst {
            case s @ [h, ...t] => h
            case _             => -1
        };
        // s should equal the original list; verify via len().
        var slen = match lst {
            case s @ [h, ...t] => len(s)
            case _             => -1
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("head"), 10);
    expect_num(*h.getGlobal("slen"), 3);
}

// ---------------------------------------------------------------------------
// Zero-field constructor
// ---------------------------------------------------------------------------

TEST_F(AtBindingTest, ZeroFieldConstructor) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Option { Some(v) None }
        var a = match Option.Some(7) {
            case n @ Option.Some(v) => v
            case n @ Option.None    => -1
        };
        var b = match Option.None() {
            case n @ Option.Some(v) => v
            case n @ Option.None    => 0
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("a"), 7);
    expect_num(*h.getGlobal("b"), 0);
}

// ---------------------------------------------------------------------------
// Or-pattern with @-binding (same names across alternatives)
// ---------------------------------------------------------------------------

TEST_F(AtBindingTest, OrPatternSameBindings) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Result { Ok(value) Err(msg) }
        fun tag_val(r) {
            return match r {
                case v @ Result.Ok(x) or v @ Result.Err(x) => x
            };
        }
        var a = tag_val(Result.Ok(1));
        var b = tag_val(Result.Err(2));
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("a"), 1);
    expect_num(*h.getGlobal("b"), 2);
}

// ---------------------------------------------------------------------------
// Guard references both @-name and field binding
// ---------------------------------------------------------------------------

TEST_F(AtBindingTest, GuardRefsBothBindings) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Result { Ok(value) Err(msg) }
        var x = match Result.Ok(5) {
            case n @ Result.Ok(v) if v > 3 => 1
            case n @ Result.Ok(v)          => 2
            case _                  => 3
        };
        var y = match Result.Ok(1) {
            case n @ Result.Ok(v) if v > 3 => 1
            case n @ Result.Ok(v)          => 2
            case _                  => 3
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("x"), 1);
    expect_num(*h.getGlobal("y"), 2);
}

// ---------------------------------------------------------------------------
// Stack neutrality: @-binding inside a loop (result captured in var first,
// then accumulated — same pattern as the existing LoopLocalSlotCorruption
// test).
// ---------------------------------------------------------------------------

TEST_F(AtBindingTest, StackNeutralInLoop) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Wrap { Val(n) }
        var total = 0;
        var i = 0;
        while (i < 3) {
            var r = match Wrap.Val(i) {
                case w @ Wrap.Val(v) => v
                case _          => 0
            };
            total = total + r;
            i = i + 1;
        }
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("total"), 3);
}

// ---------------------------------------------------------------------------
// Scope isolation: @-name not visible after the arm
// ---------------------------------------------------------------------------

TEST_F(AtBindingTest, ScopeIsolation) {
    VMTestHarness h;
    // 'n' from the first arm must not bleed into a second match.
    ASSERT_EQ(h.run(R"(
        enum Box { B(x) }
        var r1 = match Box.B(1) {
            case n @ Box.B(x) => x
            case _        => 0
        };
        var r2 = match Box.B(2) {
            case n @ Box.B(x) => x
            case _        => 0
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("r1"), 1);
    expect_num(*h.getGlobal("r2"), 2);
}

// ---------------------------------------------------------------------------
// Error: '_' as @-binding name
// ---------------------------------------------------------------------------

TEST_F(AtBindingTest, ErrorWildcardAtName) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Box { B(x) }
        var r = match Box.B(1) {
            case _ @ Box.B(x) => x
            case _        => 0
        };
    )"),
              InterpretResult::COMPILE_ERROR);
}

// ---------------------------------------------------------------------------
// Error: plain binding as sub-pattern
// ---------------------------------------------------------------------------

TEST_F(AtBindingTest, ErrorPlainBindingSubPat) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Box { B(x) }
        var r = match Box.B(1) {
            case a @ b => 0
        };
    )"),
              InterpretResult::COMPILE_ERROR);
}
