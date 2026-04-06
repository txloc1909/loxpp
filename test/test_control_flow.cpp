// test_control_flow.cpp — control flow invariant tests.
//
// Invariants:
//   1. Stack-neutral branches  — stackDepth() identical on both sides of
//   if/else.
//   2. Short-circuit semantics — and/or leave exactly one value on the stack.
//   3. Loop stack discipline   — stackDepth() same before and after while/for.
//   4. For-loop var scoping    — var in for-init is scoped to the loop.
//   5. Break exits innermost loop only; stack remains neutral.
//   6. Continue targets condition (while) or increment (for); stack neutral.

#include "test_harness.h"
#include <gtest/gtest.h>
#include <cmath>

static void expect_num(const Value& v, double expected) {
    ASSERT_TRUE(std::holds_alternative<Number>(v)) << "expected Number";
    EXPECT_NEAR(std::get<Number>(v), expected, 1e-9);
}

static void expect_bool(const Value& v, bool expected) {
    ASSERT_TRUE(std::holds_alternative<bool>(v)) << "expected bool";
    EXPECT_EQ(std::get<bool>(v), expected);
}

// ===========================================================================
// IfElse
// ===========================================================================

class IfElseTest : public ::testing::Test {};

TEST_F(IfElseTest, IfTrueBranchExecutes) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = 0; if (true) { x = 1; }"), InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 1);
}

TEST_F(IfElseTest, IfFalseBranchSkipped) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = 0; if (false) { x = 1; }"), InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 0);
}

TEST_F(IfElseTest, ElseBranchExecutes) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = 0; if (false) { x = 1; } else { x = 2; }"),
              InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 2);
}

TEST_F(IfElseTest, ElseBranchSkippedWhenTrue) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = 0; if (true) { x = 1; } else { x = 2; }"),
              InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 1);
}

TEST_F(IfElseTest, NestedIfBothTrue) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = 0; if (true) { if (true) { x = 1; } }"),
              InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 1);
}

TEST_F(IfElseTest, NestedIfOuterFalse) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = 0; if (false) { if (true) { x = 1; } }"),
              InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 0);
}

TEST_F(IfElseTest, StackNeutralAfterIf) {
    VMTestHarness h;
    h.run("if (true) { var x = 1; } else { var y = 2; }");
    EXPECT_EQ(h.stackDepth(), 0);
}

// ===========================================================================
// Logical operators (short-circuit)
// ===========================================================================

class LogicalTest : public ::testing::Test {};

TEST_F(LogicalTest, AndShortCircuitsOnFalse) {
    // Right side assigns to 'side'; it must not execute when left is false.
    VMTestHarness h;
    ASSERT_EQ(h.run("var side = 0; false and (side = 1);"),
              InterpretResult::OK);
    auto v = h.getGlobal("side");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 0);
}

TEST_F(LogicalTest, AndReturnsRightOnTrue) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var r = true and 42;"), InterpretResult::OK);
    auto v = h.getGlobal("r");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 42);
}

TEST_F(LogicalTest, OrShortCircuitsOnTrue) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var side = 0; true or (side = 1);"), InterpretResult::OK);
    auto v = h.getGlobal("side");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 0);
}

TEST_F(LogicalTest, OrReturnsRightOnFalse) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var r = false or 42;"), InterpretResult::OK);
    auto v = h.getGlobal("r");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 42);
}

TEST_F(LogicalTest, AndOrPrecedence) {
    // 'and' binds tighter: false or (true and false) == false or false == false
    VMTestHarness h;
    ASSERT_EQ(h.run("var r = false or true and false;"), InterpretResult::OK);
    auto v = h.getGlobal("r");
    ASSERT_TRUE(v.has_value());
    expect_bool(*v, false);
}

// ===========================================================================
// While loop
// ===========================================================================

class WhileLoopTest : public ::testing::Test {};

TEST_F(WhileLoopTest, WhileRunsNTimes) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var n = 0; while (n < 5) { n = n + 1; }"),
              InterpretResult::OK);
    auto v = h.getGlobal("n");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 5);
}

TEST_F(WhileLoopTest, WhileSkipsOnFalseCondition) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = 0; while (false) { x = 1; }"),
              InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 0);
}

TEST_F(WhileLoopTest, WhileStackNeutral) {
    VMTestHarness h;
    h.run("var n = 0; while (n < 3) { n = n + 1; }");
    EXPECT_EQ(h.stackDepth(), 0);
}

// ===========================================================================
// For loop
// ===========================================================================

class ForLoopTest : public ::testing::Test {};

TEST_F(ForLoopTest, ForLoopBasic) {
    // sum = 0 + 1 + 2 + 3 = 6
    VMTestHarness h;
    ASSERT_EQ(h.run("var sum = 0; for (var i = 0; i < 4; i = i + 1) { sum = "
                    "sum + i; }"),
              InterpretResult::OK);
    auto v = h.getGlobal("sum");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 6);
}

TEST_F(ForLoopTest, ForLoopNoInit) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var i = 0; var sum = 0; for (; i < 4; i = i + 1) { sum = "
                    "sum + i; }"),
              InterpretResult::OK);
    auto v = h.getGlobal("sum");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 6);
}

TEST_F(ForLoopTest, ForLoopNoIncrement) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var sum = 0; for (var i = 0; i < 4;) { sum = sum + i; i = "
                    "i + 1; }"),
              InterpretResult::OK);
    auto v = h.getGlobal("sum");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 6);
}

TEST_F(ForLoopTest, ForLoopStackNeutral) {
    VMTestHarness h;
    h.run("var sum = 0; for (var i = 0; i < 3; i = i + 1) { sum = sum + 1; }");
    EXPECT_EQ(h.stackDepth(), 0);
}

TEST_F(ForLoopTest, ForInitVarScopedToLoop) {
    // 'i' declared in for-init is not accessible after the loop.
    VMTestHarness h;
    auto result = h.run("for (var i = 0; i < 1; i = i + 1) {} print i;");
    EXPECT_EQ(result, InterpretResult::RUNTIME_ERROR);
}

// ===========================================================================
// Break
// ===========================================================================

class BreakTest : public ::testing::Test {};

TEST_F(BreakTest, BreakExitsWhileEarly) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var n = 0;
        while (true) {
            n = n + 1;
            if (n == 3) break;
        }
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("n");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 3);
}

TEST_F(BreakTest, BreakInForLoop) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var sum = 0;
        for (var i = 0; i < 10; i = i + 1) {
            if (i == 4) break;
            sum = sum + 1;
        }
    )"),
              InterpretResult::OK);
    // increments for i = 0,1,2,3 → sum = 4
    auto v = h.getGlobal("sum");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 4);
}

TEST_F(BreakTest, BreakOnlyExitsInnermostLoop) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var outer = 0;
        while (outer < 3) {
            var inner = 0;
            while (true) {
                inner = inner + 1;
                if (inner == 2) break;
            }
            outer = outer + 1;
        }
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("outer");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 3);
}

TEST_F(BreakTest, BreakCleansUpLocals) {
    VMTestHarness h;
    h.run(R"(
        while (true) {
            var x = 1;
            break;
        }
    )");
    EXPECT_EQ(h.stackDepth(), 0);
}

TEST_F(BreakTest, BreakOutsideLoopIsError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("break;"), InterpretResult::COMPILE_ERROR);
}

// ===========================================================================
// Continue
// ===========================================================================

class ContinueTest : public ::testing::Test {};

TEST_F(ContinueTest, ContinueSkipsRestOfBody) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var n = 0;
        var skipped = 0;
        while (n < 5) {
            n = n + 1;
            if (n == 3) continue;
            skipped = skipped + 1;
        }
    )"),
              InterpretResult::OK);
    // body runs 5 times, skipped once when n==3 → skipped = 4
    auto v = h.getGlobal("skipped");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 4);
}

TEST_F(ContinueTest, ContinueInForRunsIncrement) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var count = 0;
        for (var i = 0; i < 5; i = i + 1) {
            if (i == 2) continue;
            count = count + 1;
        }
    )"),
              InterpretResult::OK);
    // loop runs i=0..4 (5 iterations), skips body once → count = 4
    auto v = h.getGlobal("count");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 4);
}

TEST_F(ContinueTest, ContinueOnlyAffectsInnermostLoop) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var outer = 0;
        var total = 0;
        while (outer < 3) {
            outer = outer + 1;
            var inner = 0;
            while (inner < 4) {
                inner = inner + 1;
                if (inner == 2) continue;
                total = total + 1;
            }
        }
    )"),
              InterpretResult::OK);
    // inner runs 4 iters per outer, skips once per outer → 3 per outer, 9 total
    auto v = h.getGlobal("total");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 9);
}

TEST_F(ContinueTest, ContinueCleansUpLocals) {
    VMTestHarness h;
    h.run(R"(
        var n = 0;
        while (n < 3) {
            n = n + 1;
            var x = 1;
            continue;
        }
    )");
    EXPECT_EQ(h.stackDepth(), 0);
}

TEST_F(ContinueTest, ContinueOutsideLoopIsError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("continue;"), InterpretResult::COMPILE_ERROR);
}

// SwitchTest invariants:
//   7. Subject is evaluated exactly once.
//   8. Exactly one arm executes (the first matching one), or default if none
//   match.
//   9. Stack is neutral after the entire switch statement.
//  10. break inside a switch exits only the switch, not any enclosing loop.
//  11. continue inside a switch targets the nearest enclosing loop.
//  12. continue with no enclosing loop is a compile error.
//  13. Multiple default labels are a compile error.

class SwitchTest : public ::testing::Test {};

TEST_F(SwitchTest, MatchesFirstArm) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = 0;
        switch (2) {
            case 1: x = 1;
            case 2: x = 2;
            case 3: x = 3;
        }
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 2);
}

TEST_F(SwitchTest, SkipsNonMatchingArms) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = 0;
        switch (1) {
            case 2: x = 99;
            case 3: x = 99;
        }
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 0);
}

TEST_F(SwitchTest, DefaultExecutesWhenNoMatch) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = 0;
        switch (99) {
            case 1: x = 1;
            default: x = 42;
        }
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 42);
}

TEST_F(SwitchTest, DefaultSkippedWhenMatched) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = 0;
        switch (1) {
            case 1: x = 1;
            default: x = 99;
        }
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 1);
}

TEST_F(SwitchTest, MultiValueArm) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = 0;
        switch (2) {
            case 1, 2, 3: x = 7;
            case 4: x = 99;
        }
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 7);
}

TEST_F(SwitchTest, BreakExitsSwitch) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = 0;
        switch (1) {
            case 1: x = 1; break; x = 99;
        }
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 1);
}

TEST_F(SwitchTest, BreakInSwitchDoesNotExitLoop) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var count = 0;
        var i = 0;
        while (i < 3) {
            i = i + 1;
            switch (i) {
                case 2: break;
                default: count = count + 1;
            }
        }
    )"),
              InterpretResult::OK);
    // Iterations: i=1 → default (count=1), i=2 → break switch (count stays 1),
    // i=3 → default (count=2)
    auto v = h.getGlobal("count");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 2);
}

TEST_F(SwitchTest, ContinueInSwitchContinuesLoop) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var count = 0;
        var i = 0;
        while (i < 3) {
            i = i + 1;
            switch (i) {
                case 2: continue;
                default: count = count + 1;
            }
            count = count + 10;
        }
    )"),
              InterpretResult::OK);
    // i=1: default(count=1), then +10 → 11; i=2: continue skips +10 → 11; i=3:
    // default(count=12), +10 → 22
    auto v = h.getGlobal("count");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 22);
}

TEST_F(SwitchTest, StackNeutralAfterSwitch) {
    VMTestHarness h;
    h.run(R"(
        switch (1) {
            case 1: var x = 10;
            case 2: var y = 20;
        }
    )");
    EXPECT_EQ(h.stackDepth(), 0);
}

TEST_F(SwitchTest, SwitchOnString) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = "no";
        switch ("hello") {
            case "world": x = "world";
            case "hello": x = "hello";
        }
    )"),
              InterpretResult::OK);
    auto v = h.getGlobalStr("x");
    EXPECT_EQ(v, "hello");
}

TEST_F(SwitchTest, MultipleDefaultIsError) {
    VMTestHarness h;
    EXPECT_EQ(h.run(R"(
        switch (1) {
            default: 1;
            default: 2;
        }
    )"),
              InterpretResult::COMPILE_ERROR);
}

TEST_F(SwitchTest, ContinueWithNoEnclosingLoopIsError) {
    VMTestHarness h;
    EXPECT_EQ(h.run(R"(
        switch (1) {
            case 1: continue;
        }
    )"),
              InterpretResult::COMPILE_ERROR);
}
