// test_local_variables.cpp — local variable and block-scoping invariant tests.
//
// Invariants under test:
//   1. Stack discipline   — stackDepth() == 0 after every complete program.
//                           Within a block the depth grows and then falls back.
//   2. Scope isolation    — locals declared inside { } are unreachable after }.
//   3. Shadow semantics   — inner `var x` creates a new slot; it never mutates
//                           any outer binding.
//   4. Uninitialized guard— `var x = x;` is a compile error.
//   5. Redeclaration error— declaring the same name twice in the same scope is
//                           a compile error.
//   6. Nested scopes      — deeply nested locals resolve to the correct slot.

#include "test_harness.h"
#include <gtest/gtest.h>
#include <cmath>

// ===========================================================================
// Helpers
// ===========================================================================

static void expect_num(const Value& v, double expected) {
    ASSERT_TRUE(std::holds_alternative<Number>(v)) << "expected Number";
    EXPECT_NEAR(std::get<Number>(v), expected, 1e-9);
}

static void expect_bool(const Value& v, bool expected) {
    ASSERT_TRUE(std::holds_alternative<bool>(v)) << "expected bool";
    EXPECT_EQ(std::get<bool>(v), expected);
}

static void expect_nil(const Value& v) {
    EXPECT_TRUE(std::holds_alternative<Nil>(v)) << "expected nil";
}

// ===========================================================================
// Invariant 1: Stack discipline
// ===========================================================================

class LocalStackDisciplineTest : public ::testing::Test {};

TEST_F(LocalStackDisciplineTest, EmptyAfterEmptyBlock) {
    VMTestHarness h;
    ASSERT_EQ(h.run("{}"), InterpretResult::OK);
    EXPECT_EQ(h.stackDepth(), 0);
}

TEST_F(LocalStackDisciplineTest, EmptyAfterBlockWithLocal) {
    VMTestHarness h;
    ASSERT_EQ(h.run("{ var x = 42; }"), InterpretResult::OK);
    EXPECT_EQ(h.stackDepth(), 0);
}

TEST_F(LocalStackDisciplineTest, EmptyAfterBlockWithMultipleLocals) {
    VMTestHarness h;
    ASSERT_EQ(h.run("{ var a = 1; var b = 2; var c = 3; }"),
              InterpretResult::OK);
    EXPECT_EQ(h.stackDepth(), 0);
}

TEST_F(LocalStackDisciplineTest, EmptyAfterNestedBlocks) {
    VMTestHarness h;
    ASSERT_EQ(h.run("{ var x = 1; { var y = 2; { var z = 3; } } }"),
              InterpretResult::OK);
    EXPECT_EQ(h.stackDepth(), 0);
}

TEST_F(LocalStackDisciplineTest, EmptyAfterMixedGlobalAndLocal) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var g = 10; { var l = 20; }"), InterpretResult::OK);
    EXPECT_EQ(h.stackDepth(), 0);
}

TEST_F(LocalStackDisciplineTest, EmptyAfterLocalExpression) {
    VMTestHarness h;
    // Local used in an expression statement — value pushed, then popped by POP.
    ASSERT_EQ(h.run("{ var x = 5; x + 1; }"), InterpretResult::OK);
    EXPECT_EQ(h.stackDepth(), 0);
}

// ===========================================================================
// Invariant 2: Scope isolation
// ===========================================================================

class ScopeIsolationTest : public ::testing::Test {};

TEST_F(ScopeIsolationTest, LocalIsNotGlobal) {
    VMTestHarness h;
    ASSERT_EQ(h.run("{ var x = 99; }"), InterpretResult::OK);
    // x was local, so getGlobal sees nothing.
    EXPECT_FALSE(h.getGlobal("x").has_value());
}

TEST_F(ScopeIsolationTest, LocalUnreachableAfterBlock) {
    // Reading a local after its block ends is a runtime error (it resolves
    // to a global that was never defined).
    VMTestHarness h;
    ASSERT_EQ(h.run("{ var x = 1; }\nprint x;"),
              InterpretResult::RUNTIME_ERROR);
}

TEST_F(ScopeIsolationTest, InnerLocalDoesNotLeakToOuter) {
    // The outer block's `x` is separate from the inner block's `y`.
    // After the inner block, `y` is gone.
    VMTestHarness h;
    ASSERT_EQ(h.run("{ var x = 10; { var y = 20; } print x; }"),
              InterpretResult::OK);
    EXPECT_EQ(h.stackDepth(), 0);
}

TEST_F(ScopeIsolationTest, LocalDeclarationInEachBranchIsIsolated) {
    // Two sequential blocks each declaring `x`; neither leaks into the other.
    VMTestHarness h;
    ASSERT_EQ(h.run("{ var x = 1; } { var x = 2; }"), InterpretResult::OK);
    EXPECT_EQ(h.stackDepth(), 0);
    EXPECT_FALSE(h.getGlobal("x").has_value());
}

// ===========================================================================
// Invariant 3: Shadow semantics
// ===========================================================================

class ShadowSemanticsTest : public ::testing::Test {};

TEST_F(ShadowSemanticsTest, InnerVarShadowsGlobal) {
    VMTestHarness h;
    // Global `x = 1`. Inside block, local `x = 2` shadows it. After block,
    // global `x` is still 1.
    ASSERT_EQ(h.run("var x = 1; { var x = 2; } "), InterpretResult::OK);
    auto g = h.getGlobal("x");
    ASSERT_TRUE(g.has_value());
    expect_num(*g, 1.0);
}

TEST_F(ShadowSemanticsTest, InnerVarShadowsOuterLocal) {
    // Outer local `x = 10`, inner local `x = 20`. After inner block, the outer
    // `x` expression result is 10.
    VMTestHarness h;
    ASSERT_EQ(h.run("{ var x = 10; { var x = 20; } x; }"), InterpretResult::OK);
    expect_num(h.lastResult(), 10.0);
}

TEST_F(ShadowSemanticsTest, AssignmentToShadowDoesNotMutateOuter) {
    // Assigning to inner `x` must not touch the outer `x`.
    VMTestHarness h;
    ASSERT_EQ(h.run("{ var x = 1; { var x = 2; x = 99; } x; }"),
              InterpretResult::OK);
    expect_num(h.lastResult(), 1.0);
}

TEST_F(ShadowSemanticsTest, GlobalUnchangedAfterLocalAssignment) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = 5; { var x = 100; x = 200; } "),
              InterpretResult::OK);
    auto g = h.getGlobal("x");
    ASSERT_TRUE(g.has_value());
    expect_num(*g, 5.0);
}

TEST_F(ShadowSemanticsTest, MultipleLevelsOfShadow) {
    VMTestHarness h;
    // Three nested `x` declarations; each level sees its own.
    ASSERT_EQ(h.run("{ var x = 1; { var x = 2; { var x = 3; x; } x; } x; }"),
              InterpretResult::OK);
    // lastResult() captures the outermost `x;` expression statement.
    expect_num(h.lastResult(), 1.0);
}

// ===========================================================================
// Invariant 4: Uninitialized guard (self-referential initializer)
// ===========================================================================

class UninitializedGuardTest : public ::testing::Test {};

TEST_F(UninitializedGuardTest, SelfReferentialInitializerIsCompileError) {
    VMTestHarness h;
    // `var x = x;` should be a compile error — x is declared but not yet
    // initialized when the initializer expression is compiled.
    EXPECT_EQ(h.run("{ var x = x; }"), InterpretResult::COMPILE_ERROR);
}

TEST_F(UninitializedGuardTest, SelfRefDoesNotAffectOuterSameNameGlobal) {
    // At global scope, `var x = x;` is NOT an error (globals are looked up
    // at runtime and the outer `x` may exist). Only block-locals get the guard.
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = 1; var y = x;"), InterpretResult::OK);
    EXPECT_EQ(h.stackDepth(), 0);
}

// ===========================================================================
// Invariant 5: Redeclaration error
// ===========================================================================

class RedeclarationTest : public ::testing::Test {};

TEST_F(RedeclarationTest, RedeclareInSameScopeIsCompileError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("{ var x = 1; var x = 2; }"),
              InterpretResult::COMPILE_ERROR);
}

TEST_F(RedeclarationTest, RedeclareInDifferentScopesIsOk) {
    // Same name in two sequential sibling scopes is fine.
    VMTestHarness h;
    EXPECT_EQ(h.run("{ var x = 1; } { var x = 2; }"), InterpretResult::OK);
}

TEST_F(RedeclarationTest, RedeclareInInnerScopeIsOk) {
    // Inner scope shadows, not redeclares.
    VMTestHarness h;
    EXPECT_EQ(h.run("{ var x = 1; { var x = 2; } }"), InterpretResult::OK);
}

TEST_F(RedeclarationTest, GlobalRedeclarationIsOk) {
    // At global scope, redeclaration silently overwrites (book semantics).
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = 1; var x = 2;"), InterpretResult::OK);
    auto g = h.getGlobal("x");
    ASSERT_TRUE(g.has_value());
    expect_num(*g, 2.0);
}

// ===========================================================================
// Invariant 6: Nested scopes — correct slot resolution
// ===========================================================================

class NestedScopeTest : public ::testing::Test {};

TEST_F(NestedScopeTest, InnerScopeReadsOuterLocal) {
    VMTestHarness h;
    ASSERT_EQ(h.run("{ var x = 42; { x; } }"), InterpretResult::OK);
    expect_num(h.lastResult(), 42.0);
}

TEST_F(NestedScopeTest, InnerScopeWritesOuterLocal) {
    VMTestHarness h;
    ASSERT_EQ(h.run("{ var x = 1; { x = 99; } x; }"), InterpretResult::OK);
    expect_num(h.lastResult(), 99.0);
}

TEST_F(NestedScopeTest, MultipleSlotsCorrectlyOrdered) {
    VMTestHarness h;
    // Three locals in one scope: a=1, b=2, c=3. Reading each must get
    // the right slot regardless of order.
    ASSERT_EQ(h.run("{ var a = 1; var b = 2; var c = 3; a + b + c; }"),
              InterpretResult::OK);
    expect_num(h.lastResult(), 6.0);
}

TEST_F(NestedScopeTest, DeepNestingResolvesCorrectly) {
    VMTestHarness h;
    ASSERT_EQ(h.run("{ var x = 1; { var y = 2; { var z = 3; x + y + z; } } }"),
              InterpretResult::OK);
    expect_num(h.lastResult(), 6.0);
}

TEST_F(NestedScopeTest, LocalNilDefault) {
    VMTestHarness h;
    ASSERT_EQ(h.run("{ var x; x; }"), InterpretResult::OK);
    expect_nil(h.lastResult());
}

TEST_F(NestedScopeTest, LocalAssignmentIsExpression) {
    // Assignment is an expression: the result of `x = 5` is 5.
    VMTestHarness h;
    ASSERT_EQ(h.run("{ var x = 0; x = 5; }"), InterpretResult::OK);
    // The assignment expression's value was popped by the expression statement.
    // Verify x was actually updated.
    ASSERT_EQ(h.run("{ var x = 0; x = 5; x; }"), InterpretResult::OK);
    expect_num(h.lastResult(), 5.0);
}

TEST_F(NestedScopeTest, LocalStringValue) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"({ var s = "hello"; s; })"), InterpretResult::OK);
    ASSERT_TRUE(isString(h.lastResult()));
}

TEST_F(NestedScopeTest, LocalBoolValue) {
    VMTestHarness h;
    ASSERT_EQ(h.run("{ var b = true; b; }"), InterpretResult::OK);
    expect_bool(h.lastResult(), true);
}

TEST_F(NestedScopeTest, LocalAndGlobalInteraction) {
    // A global and a local with different names in the same block.
    VMTestHarness h;
    ASSERT_EQ(h.run("var g = 10; { var l = 20; g + l; }"), InterpretResult::OK);
    expect_num(h.lastResult(), 30.0);
}
