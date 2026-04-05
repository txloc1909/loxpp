// test_local_variables.cpp — local variable and block-scoping invariant tests.
//
// Invariants:
//   1. Stack discipline   — stackDepth() == 0 after every complete program.
//   2. Scope isolation    — locals declared inside { } are unreachable after }.
//   3. Shadow semantics   — inner `var x` never mutates any outer binding.
//   4. Uninitialized guard— `var x = x;` is a compile error.
//   5. Redeclaration error— same name twice in the same scope is a compile
//   error.
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

// ===========================================================================
// Invariant 1: Stack discipline
// ===========================================================================

class LocalStackDisciplineTest : public ::testing::TestWithParam<const char*> {
};

TEST_P(LocalStackDisciplineTest, StackEmptyAfterBlock) {
    VMTestHarness h;
    ASSERT_EQ(h.run(GetParam()), InterpretResult::OK);
    EXPECT_EQ(h.stackDepth(), 0);
}

INSTANTIATE_TEST_SUITE_P(
    StackDiscipline, LocalStackDisciplineTest,
    ::testing::Values("{}", "{ var x = 42; }",
                      "{ var a = 1; var b = 2; var c = 3; }",
                      "{ var x = 1; { var y = 2; { var z = 3; } } }",
                      "var g = 10; { var l = 20; }", "{ var x = 5; x + 1; }"));

// ===========================================================================
// Invariant 2: Scope isolation
// ===========================================================================

class ScopeIsolationTest : public ::testing::Test {};

TEST_F(ScopeIsolationTest, LocalIsNotGlobal) {
    VMTestHarness h;
    ASSERT_EQ(h.run("{ var x = 99; }"), InterpretResult::OK);
    EXPECT_FALSE(h.getGlobal("x").has_value());
}

TEST_F(ScopeIsolationTest, LocalUnreachableAfterBlock) {
    VMTestHarness h;
    ASSERT_EQ(h.run("{ var x = 1; }\nprint x;"),
              InterpretResult::RUNTIME_ERROR);
}

TEST_F(ScopeIsolationTest, InnerLocalDoesNotLeakToOuter) {
    VMTestHarness h;
    ASSERT_EQ(h.run("{ var x = 10; { var y = 20; } print x; }"),
              InterpretResult::OK);
    EXPECT_EQ(h.stackDepth(), 0);
}

TEST_F(ScopeIsolationTest, LocalDeclarationInEachBranchIsIsolated) {
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
    ASSERT_EQ(h.run("var x = 1; { var x = 2; } "), InterpretResult::OK);
    auto g = h.getGlobal("x");
    ASSERT_TRUE(g.has_value());
    expect_num(*g, 1.0);
}

TEST_F(ShadowSemanticsTest, InnerVarShadowsOuterLocal) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var result; { var x = 10; { var x = 20; } result = x; }"),
              InterpretResult::OK);
    auto r = h.getGlobal("result");
    ASSERT_TRUE(r.has_value());
    expect_num(*r, 10.0);
}

TEST_F(ShadowSemanticsTest, AssignmentToShadowDoesNotMutateOuter) {
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
    EXPECT_EQ(h.run("{ var x = x; }"), InterpretResult::COMPILE_ERROR);
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

TEST_F(RedeclarationTest, GlobalRedeclarationIsOk) {
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
