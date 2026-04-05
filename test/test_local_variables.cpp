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
    // `x` is still 10. Capture via global so endScope's POPs don't interfere.
    VMTestHarness h;
    ASSERT_EQ(h.run("var result; { var x = 10; { var x = 20; } result = x; }"),
              InterpretResult::OK);
    auto r = h.getGlobal("result");
    ASSERT_TRUE(r.has_value());
    expect_num(*r, 10.0);
}

TEST_F(ShadowSemanticsTest, AssignmentToShadowDoesNotMutateOuter) {
    // Assigning to inner `x` must not touch the outer `x`.
    VMTestHarness h;
    ASSERT_EQ(
        h.run("var result; { var x = 1; { var x = 2; x = 99; } result = x; }"),
        InterpretResult::OK);
    auto r = h.getGlobal("result");
    ASSERT_TRUE(r.has_value());
    expect_num(*r, 1.0);
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
    // Three nested `x` declarations; capture each level's value via a global.
    ASSERT_EQ(h.run("var r1; var r2; var r3;"
                    "{ var x = 1;"
                    "  { var x = 2;"
                    "    { var x = 3; r3 = x; }"
                    "    r2 = x;"
                    "  }"
                    "  r1 = x;"
                    "}"),
              InterpretResult::OK);
    auto r1 = h.getGlobal("r1");
    auto r2 = h.getGlobal("r2");
    auto r3 = h.getGlobal("r3");
    ASSERT_TRUE(r1.has_value() && r2.has_value() && r3.has_value());
    expect_num(*r1, 1.0);
    expect_num(*r2, 2.0);
    expect_num(*r3, 3.0);
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
    ASSERT_EQ(h.run("var result; { var x = 42; { result = x; } }"),
              InterpretResult::OK);
    auto r = h.getGlobal("result");
    ASSERT_TRUE(r.has_value());
    expect_num(*r, 42.0);
}

TEST_F(NestedScopeTest, InnerScopeWritesOuterLocal) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var result; { var x = 1; { x = 99; } result = x; }"),
              InterpretResult::OK);
    auto r = h.getGlobal("result");
    ASSERT_TRUE(r.has_value());
    expect_num(*r, 99.0);
}

TEST_F(NestedScopeTest, MultipleSlotsCorrectlyOrdered) {
    VMTestHarness h;
    // Three locals in one scope; capture sum via global.
    ASSERT_EQ(h.run("var result;"
                    "{ var a = 1; var b = 2; var c = 3; result = a + b + c; }"),
              InterpretResult::OK);
    auto r = h.getGlobal("result");
    ASSERT_TRUE(r.has_value());
    expect_num(*r, 6.0);
}

TEST_F(NestedScopeTest, DeepNestingResolvesCorrectly) {
    VMTestHarness h;
    ASSERT_EQ(
        h.run(
            "var result;"
            "{ var x = 1; { var y = 2; { var z = 3; result = x + y + z; } } }"),
        InterpretResult::OK);
    auto r = h.getGlobal("result");
    ASSERT_TRUE(r.has_value());
    expect_num(*r, 6.0);
}

TEST_F(NestedScopeTest, LocalNilDefault) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var result; { var x; result = x; }"), InterpretResult::OK);
    auto r = h.getGlobal("result");
    ASSERT_TRUE(r.has_value());
    expect_nil(*r);
}

TEST_F(NestedScopeTest, LocalAssignmentIsExpression) {
    // Assignment is an expression: `x = 5` produces 5, and updating x is
    // visible.
    VMTestHarness h;
    ASSERT_EQ(h.run("var result; { var x = 0; x = 5; result = x; }"),
              InterpretResult::OK);
    auto r = h.getGlobal("result");
    ASSERT_TRUE(r.has_value());
    expect_num(*r, 5.0);
}

TEST_F(NestedScopeTest, LocalStringValue) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var result; { var s = "hello"; result = s; })"),
              InterpretResult::OK);
    auto r = h.getGlobal("result");
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(isString(*r));
}

TEST_F(NestedScopeTest, LocalBoolValue) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var result; { var b = true; result = b; }"),
              InterpretResult::OK);
    auto r = h.getGlobal("result");
    ASSERT_TRUE(r.has_value());
    expect_bool(*r, true);
}

TEST_F(NestedScopeTest, LocalAndGlobalInteraction) {
    // A global and a local with different names in the same block.
    VMTestHarness h;
    ASSERT_EQ(h.run("var g = 10; var result; { var l = 20; result = g + l; }"),
              InterpretResult::OK);
    auto r = h.getGlobal("result");
    ASSERT_TRUE(r.has_value());
    expect_num(*r, 30.0);
}
