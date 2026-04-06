// test_control_flow.cpp — control flow invariant tests.
//
// Invariants:
//   1. Stack-neutral branches  — stackDepth() identical on both sides of if/else.
//   2. Short-circuit semantics — and/or leave exactly one value on the stack.
//   3. Loop stack discipline   — stackDepth() same before and after while/for.
//   4. For-loop var scoping    — var in for-init is scoped to the loop.

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
    ASSERT_EQ(h.run("var x = 0; if (false) { x = 1; } else { x = 2; }"), InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 2);
}

TEST_F(IfElseTest, ElseBranchSkippedWhenTrue) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = 0; if (true) { x = 1; } else { x = 2; }"), InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 1);
}

TEST_F(IfElseTest, NestedIfBothTrue) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = 0; if (true) { if (true) { x = 1; } }"), InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 1);
}

TEST_F(IfElseTest, NestedIfOuterFalse) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = 0; if (false) { if (true) { x = 1; } }"), InterpretResult::OK);
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
    ASSERT_EQ(h.run("var side = 0; false and (side = 1);"), InterpretResult::OK);
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
    ASSERT_EQ(h.run("var n = 0; while (n < 5) { n = n + 1; }"), InterpretResult::OK);
    auto v = h.getGlobal("n");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 5);
}

TEST_F(WhileLoopTest, WhileSkipsOnFalseCondition) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = 0; while (false) { x = 1; }"), InterpretResult::OK);
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
    ASSERT_EQ(h.run("var sum = 0; for (var i = 0; i < 4; i = i + 1) { sum = sum + i; }"),
              InterpretResult::OK);
    auto v = h.getGlobal("sum");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 6);
}

TEST_F(ForLoopTest, ForLoopNoInit) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var i = 0; var sum = 0; for (; i < 4; i = i + 1) { sum = sum + i; }"),
              InterpretResult::OK);
    auto v = h.getGlobal("sum");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 6);
}

TEST_F(ForLoopTest, ForLoopNoIncrement) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var sum = 0; for (var i = 0; i < 4;) { sum = sum + i; i = i + 1; }"),
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
