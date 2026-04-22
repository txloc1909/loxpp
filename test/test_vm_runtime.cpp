// test_vm_runtime.cpp — VM runtime state & global variable invariant tests.
//
// Invariants under test:
//   1. Stack discipline   — stackDepth() == 0 after every complete program.
//   2. Defined-before-use — reading an undefined global is always
//   RUNTIME_ERROR.
//   3. DEFINE_GLOBAL      — var x; stores nil; var x = v; stores v; re-decl
//   overwrites.
//   4. SET_GLOBAL         — assignment leaves value on stack (expression
//   semantics).
//   5. Globals canonical  — getGlobal() is the sole source of truth; no stale
//   reads.

#include "test_harness.h"
#include <gtest/gtest.h>
#include <cmath>
#include <optional>

// ===========================================================================
// Helpers
// ===========================================================================

static void expect_global_num(VMTestHarness& h, const std::string& name,
                              double expected) {
    auto v = h.getGlobal(name);
    ASSERT_TRUE(v.has_value()) << "  global '" << name << "' not defined";
    ASSERT_TRUE(is<Number>(*v)) << "  global '" << name << "' expected Number";
    EXPECT_NEAR(as<Number>(*v), expected, 1e-9);
}

static void expect_global_bool(VMTestHarness& h, const std::string& name,
                               bool expected) {
    auto v = h.getGlobal(name);
    ASSERT_TRUE(v.has_value()) << "  global '" << name << "' not defined";
    ASSERT_TRUE(is<bool>(*v)) << "  global '" << name << "' expected bool";
    EXPECT_EQ(as<bool>(*v), expected);
}

static void expect_global_nil(VMTestHarness& h, const std::string& name) {
    auto v = h.getGlobal(name);
    ASSERT_TRUE(v.has_value()) << "  global '" << name << "' not defined";
    EXPECT_TRUE(is<Nil>(*v)) << "  global '" << name << "' expected nil";
}

static void expect_global_str(VMTestHarness& h, const std::string& name,
                              const std::string& expected) {
    EXPECT_EQ(h.getGlobalStr(name), expected);
}

static void expect_global_absent(VMTestHarness& h, const std::string& name) {
    EXPECT_FALSE(h.getGlobal(name).has_value())
        << "  global '" << name << "' should be absent (undefined)";
}

// ===========================================================================
// Invariant 1: Stack discipline
// ===========================================================================

class StackDisciplineTest : public ::testing::Test {};

// After any complete, well-formed program the stack must be empty.
TEST_F(StackDisciplineTest, EmptyAfterExpressionStatement) {
    VMTestHarness h;
    ASSERT_EQ(h.run("1 + 2;"), InterpretResult::OK);
    EXPECT_EQ(h.stackDepth(), 0);
}

TEST_F(StackDisciplineTest, EmptyAfterPrintStatement) {
    VMTestHarness h;
    ASSERT_EQ(h.run("print 42;"), InterpretResult::OK);
    EXPECT_EQ(h.stackDepth(), 0);
}

TEST_F(StackDisciplineTest, EmptyAfterVarDeclaration) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = 10;"), InterpretResult::OK);
    EXPECT_EQ(h.stackDepth(), 0);
}

TEST_F(StackDisciplineTest, EmptyAfterMultipleStatements) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var a = 1;\nvar b = 2;\nprint a + b;"),
              InterpretResult::OK);
    EXPECT_EQ(h.stackDepth(), 0);
}

TEST_F(StackDisciplineTest, EmptyAfterAssignmentExpression) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = 0;\nx = 5;"), InterpretResult::OK);
    EXPECT_EQ(h.stackDepth(), 0);
}

TEST_F(StackDisciplineTest, EmptyAfterRuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("var x;\nx;"), InterpretResult::OK);
    // Undefined variable read is a runtime error — stack is reset.
    EXPECT_EQ(h.run("undeclared;"), InterpretResult::RUNTIME_ERROR);
    EXPECT_EQ(h.stackDepth(), 0);
}

// ===========================================================================
// Invariant 2: Defined-before-use (RUNTIME_ERROR for undefined globals)
// ===========================================================================

class DefinedBeforeUseTest : public ::testing::Test {};

TEST_F(DefinedBeforeUseTest, ReadUndefinedGlobal_IsRuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("notDefined;"), InterpretResult::RUNTIME_ERROR);
}

TEST_F(DefinedBeforeUseTest, AssignUndefinedGlobal_IsRuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("notDefined = 42;"), InterpretResult::RUNTIME_ERROR);
}

TEST_F(DefinedBeforeUseTest, UndefinedGlobal_IsAbsentFromInspection) {
    VMTestHarness h;
    expect_global_absent(h, "ghost");
}

TEST_F(DefinedBeforeUseTest, DefinedGlobal_IsPresent) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = 1;"), InterpretResult::OK);
    EXPECT_TRUE(h.getGlobal("x").has_value());
}

// ===========================================================================
// Invariant 3: DEFINE_GLOBAL semantics
// ===========================================================================

class DefineGlobalTest : public ::testing::Test {};

TEST_F(DefineGlobalTest, UninitializedVarStoresNil) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var x;"), InterpretResult::OK);
    expect_global_nil(h, "x");
}

TEST_F(DefineGlobalTest, InitializedVarStoresValue) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var answer = 42;"), InterpretResult::OK);
    expect_global_num(h, "answer", 42.0);
}

TEST_F(DefineGlobalTest, BoolVar) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var flag = true;"), InterpretResult::OK);
    expect_global_bool(h, "flag", true);
}

TEST_F(DefineGlobalTest, StringVar) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var greeting = \"hello\";"), InterpretResult::OK);
    expect_global_str(h, "greeting", "hello");
}

TEST_F(DefineGlobalTest, ExpressionInitializer) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = 2 + 3 * 4;"), InterpretResult::OK);
    expect_global_num(h, "x", 14.0);
}

// Re-declaration at global scope silently overwrites (Lox semantics).
TEST_F(DefineGlobalTest, RedeclarationOverwrites) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = 1;\nvar x = 99;"), InterpretResult::OK);
    expect_global_num(h, "x", 99.0);
}

TEST_F(DefineGlobalTest, MultipleVars) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var a = 1;\nvar b = 2;\nvar c = 3;"), InterpretResult::OK);
    expect_global_num(h, "a", 1.0);
    expect_global_num(h, "b", 2.0);
    expect_global_num(h, "c", 3.0);
}

// ===========================================================================
// Invariant 4 & 5: SET_GLOBAL / GET_GLOBAL and canonical globals table
// ===========================================================================

class GetSetGlobalTest : public ::testing::Test {};

TEST_F(GetSetGlobalTest, ReadAfterDefine) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = 7;\nvar y = x;"), InterpretResult::OK);
    expect_global_num(h, "x", 7.0);
    expect_global_num(h, "y", 7.0);
}

TEST_F(GetSetGlobalTest, AssignmentUpdatesGlobals) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = 1;\nx = 2;"), InterpretResult::OK);
    expect_global_num(h, "x", 2.0);
}

TEST_F(GetSetGlobalTest, AssignmentChain) {
    VMTestHarness h;
    // Assignment is an expression; chaining reads/writes the same global.
    ASSERT_EQ(h.run("var x = 0;\nvar y = 0;\nx = y = 5;"), InterpretResult::OK);
    expect_global_num(h, "x", 5.0);
    expect_global_num(h, "y", 5.0);
}

// Invariant 4: SET_GLOBAL is an expression — the value stays on stack until
// the enclosing statement's POP. After the statement, stack must be 0.
TEST_F(GetSetGlobalTest, AssignmentExpressionLeavesStackClean) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = 0;\nx = 10;"), InterpretResult::OK);
    EXPECT_EQ(h.stackDepth(), 0);
    expect_global_num(h, "x", 10.0);
}

TEST_F(GetSetGlobalTest, AssignmentResultUsableInExpression) {
    VMTestHarness h;
    // (x = 5) is an expression that evaluates to 5.
    ASSERT_EQ(h.run("var x = 0;\nvar y = (x = 5);"), InterpretResult::OK);
    expect_global_num(h, "x", 5.0);
    expect_global_num(h, "y", 5.0);
}

// Invariant 5: getGlobal() is always up-to-date; multiple writes are visible.
TEST_F(GetSetGlobalTest, MultipleAssignmentsAllVisible) {
    VMTestHarness h;
    ASSERT_EQ(
        h.run("var counter = 0;\ncounter = 1;\ncounter = 2;\ncounter = 3;"),
        InterpretResult::OK);
    expect_global_num(h, "counter", 3.0);
}

TEST_F(GetSetGlobalTest, GlobalUsedInArithmetic) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = 10;\nvar y = x * 2 + 1;"), InterpretResult::OK);
    expect_global_num(h, "y", 21.0);
}

// ===========================================================================
// Compile errors
// ===========================================================================

class CompileErrorTest : public ::testing::Test {};

TEST_F(CompileErrorTest, VarMissingName) {
    VMTestHarness h;
    EXPECT_EQ(h.run("var = 1;"), InterpretResult::COMPILE_ERROR);
}

TEST_F(CompileErrorTest, VarMissingSemicolon) {
    VMTestHarness h;
    EXPECT_EQ(h.run("var x = 1"), InterpretResult::COMPILE_ERROR);
}

TEST_F(CompileErrorTest, InvalidAssignmentTarget) {
    VMTestHarness h;
    // Assigning to a non-lvalue: literal on the left of '='.
    EXPECT_EQ(h.run("1 + 2 = 3;"), InterpretResult::COMPILE_ERROR);
}

TEST_F(CompileErrorTest, TooManyConstants) {
    // Generate 256 distinct numeric literal expression statements.
    // The 256th constant exhausts the pool (capacity: 255) and must produce a
    // compile error — not a crash or out-of-bounds exception.
    std::string source;
    for (int i = 0; i < 256; ++i)
        source += std::to_string(i) + ";\n";
    VMTestHarness h;
    EXPECT_EQ(h.run(source), InterpretResult::COMPILE_ERROR);
}

// ===========================================================================
// Call frame tests
// ===========================================================================
// These tests verify the VM's call frame machinery via Op::CALL. Since there
// is no function declaration syntax yet, we exercise Op::CALL by calling
// non-function values (which must produce a runtime error) and verify that
// the stack is clean afterwards.

class CallFrameTest : public ::testing::Test {};

// Calling a number must produce a runtime error.
TEST_F(CallFrameTest, CallNumberIsRuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("var x = 42;\nx();"), InterpretResult::RUNTIME_ERROR);
}

// Calling nil must produce a runtime error.
TEST_F(CallFrameTest, CallNilIsRuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("nil();"), InterpretResult::RUNTIME_ERROR);
}

// Calling a boolean must produce a runtime error.
TEST_F(CallFrameTest, CallBoolIsRuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("true();"), InterpretResult::RUNTIME_ERROR);
}

// Calling a string must produce a runtime error.
TEST_F(CallFrameTest, CallStringIsRuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("\"hello\"();"), InterpretResult::RUNTIME_ERROR);
}

// Stack must be empty after a runtime error during a call attempt.
TEST_F(CallFrameTest, StackCleanAfterCallError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("nil();"), InterpretResult::RUNTIME_ERROR);
    EXPECT_EQ(h.stackDepth(), 0);
}

// A call with arguments on a non-function is still a runtime error.
TEST_F(CallFrameTest, CallNilWithArgsIsRuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("nil(1, 2, 3);"), InterpretResult::RUNTIME_ERROR);
}

// Calling a computed expression that yields a non-function is an error.
TEST_F(CallFrameTest, CallExpressionResultIsRuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("(1 + 2)();"), InterpretResult::RUNTIME_ERROR);
}

// ===========================================================================
// Function declaration and call tests
// ===========================================================================

class FunctionTest : public ::testing::Test {};

// A function with no explicit return returns nil.
TEST_F(FunctionTest, NoExplicitReturn_ReturnsNil) {
    VMTestHarness h;
    ASSERT_EQ(h.run("fun f() {} f();"), InterpretResult::OK);
    EXPECT_EQ(h.lastResult(), from<Nil>(Nil{}));
}

// A function can return a string literal.
TEST_F(FunctionTest, ReturnStringLiteral) {
    VMTestHarness h;
    ASSERT_EQ(h.run("fun greet() { return \"hello\"; } greet();"),
              InterpretResult::OK);
    EXPECT_EQ(stringify(h.lastResult()), "hello");
}

// A function can return a computed value.
TEST_F(FunctionTest, ReturnComputedValue) {
    VMTestHarness h;
    ASSERT_EQ(h.run("fun add(a, b) { return a + b; } add(2, 3);"),
              InterpretResult::OK);
    EXPECT_EQ(h.lastResult(), from<Number>(5.0));
}

// Calling a function with too few arguments is a runtime error.
TEST_F(FunctionTest, TooFewArguments_RuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("fun f(x) {} f();"), InterpretResult::RUNTIME_ERROR);
}

// Calling a function with too many arguments is a runtime error.
TEST_F(FunctionTest, TooManyArguments_RuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("fun f() {} f(1);"), InterpretResult::RUNTIME_ERROR);
}

// A function can call itself recursively.
TEST_F(FunctionTest, Recursion) {
    VMTestHarness h;
    std::string src = "fun count(n) {"
                      "  if (n <= 0) return 0;"
                      "  return count(n - 1) + 1;"
                      "}"
                      "count(5);";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.lastResult(), from<Number>(5.0));
}

// Two mutually independent functions can coexist.
TEST_F(FunctionTest, TwoFunctions) {
    VMTestHarness h;
    std::string src = "fun double(x) { return x * 2; }"
                      "fun triple(x) { return x * 3; }"
                      "double(3) + triple(2);";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.lastResult(), from<Number>(12.0));
}

// return at the top level is a compile error.
TEST_F(FunctionTest, ReturnAtTopLevel_CompileError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("return 1;"), InterpretResult::COMPILE_ERROR);
}

// Stack must be empty after a successful function call.
TEST_F(FunctionTest, StackCleanAfterCall) {
    VMTestHarness h;
    ASSERT_EQ(h.run("fun f() { return 42; } f();"), InterpretResult::OK);
    EXPECT_EQ(h.stackDepth(), 0);
}

// A function defined in a local scope is a local variable.
// Store the result globally so endScope()'s POP of `square` doesn't
// overwrite lastResult().
TEST_F(FunctionTest, LocalFunction) {
    VMTestHarness h;
    std::string src = "var r = 0;"
                      "{"
                      "  fun square(x) { return x * x; }"
                      "  r = square(4);"
                      "}"
                      "r;";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.lastResult(), from<Number>(16.0));
}

// ===========================================================================
// Native function tests
// ===========================================================================

class NativeTest : public ::testing::Test {};

// ---------------------------------------------------------------------------
// clock()
// ---------------------------------------------------------------------------

TEST_F(NativeTest, Clock_ReturnsNumber) {
    VMTestHarness h;
    ASSERT_EQ(h.run("clock();"), InterpretResult::OK);
    EXPECT_TRUE(is<Number>(h.lastResult()));
}

TEST_F(NativeTest, Clock_IsNonNegative) {
    VMTestHarness h;
    ASSERT_EQ(h.run("clock();"), InterpretResult::OK);
    EXPECT_GE(as<Number>(h.lastResult()), 0.0);
}

TEST_F(NativeTest, Clock_NonDecreasing) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var t1 = clock(); var t2 = clock(); t2 - t1;"),
              InterpretResult::OK);
    EXPECT_GE(as<Number>(h.lastResult()), 0.0);
}

TEST_F(NativeTest, Clock_WrongArity_RuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("clock(1);"), InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// str()
// ---------------------------------------------------------------------------

TEST_F(NativeTest, Str_Number) {
    VMTestHarness h;
    ASSERT_EQ(h.run("str(42);"), InterpretResult::OK);
    EXPECT_EQ(stringify(h.lastResult()), "42");
}

TEST_F(NativeTest, Str_NumberDecimal) {
    VMTestHarness h;
    ASSERT_EQ(h.run("str(3.14);"), InterpretResult::OK);
    EXPECT_EQ(stringify(h.lastResult()), "3.14");
}

TEST_F(NativeTest, Str_BoolTrue) {
    VMTestHarness h;
    ASSERT_EQ(h.run("str(true);"), InterpretResult::OK);
    EXPECT_EQ(stringify(h.lastResult()), "true");
}

TEST_F(NativeTest, Str_BoolFalse) {
    VMTestHarness h;
    ASSERT_EQ(h.run("str(false);"), InterpretResult::OK);
    EXPECT_EQ(stringify(h.lastResult()), "false");
}

TEST_F(NativeTest, Str_Nil) {
    VMTestHarness h;
    ASSERT_EQ(h.run("str(nil);"), InterpretResult::OK);
    EXPECT_EQ(stringify(h.lastResult()), "nil");
}

TEST_F(NativeTest, Str_String_IsIdentity) {
    VMTestHarness h;
    ASSERT_EQ(h.run("str(\"hello\");"), InterpretResult::OK);
    EXPECT_EQ(stringify(h.lastResult()), "hello");
}

TEST_F(NativeTest, Str_ResultIsString) {
    // str() must return an ObjString, not just a printable value.
    VMTestHarness h;
    ASSERT_EQ(h.run("str(42);"), InterpretResult::OK);
    EXPECT_TRUE(isString(h.lastResult()));
}

TEST_F(NativeTest, Str_WrongArity_TooFew_RuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("str();"), InterpretResult::RUNTIME_ERROR);
}

TEST_F(NativeTest, Str_WrongArity_TooMany_RuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("str(1, 2);"), InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// Behavioural: natives usable in expressions
// ---------------------------------------------------------------------------

TEST_F(NativeTest, Str_UsableInConcatenation) {
    VMTestHarness h;
    ASSERT_EQ(h.run("\"value: \" + str(99);"), InterpretResult::OK);
    EXPECT_EQ(stringify(h.lastResult()), "value: 99");
}

TEST_F(NativeTest, NativesAreGlobalVariables) {
    // Natives are just values stored in globals — they can be assigned to
    // local variables and called through them.
    VMTestHarness h;
    ASSERT_EQ(h.run("var s = str; s(7);"), InterpretResult::OK);
    EXPECT_EQ(stringify(h.lastResult()), "7");
}

TEST_F(NativeTest, StackCleanAfterNativeCall) {
    VMTestHarness h;
    ASSERT_EQ(h.run("str(1); str(2); str(3);"), InterpretResult::OK);
    EXPECT_EQ(h.stackDepth(), 0);
}

// ===========================================================================
// Modulo operator tests
// ===========================================================================

class ModuloTest : public ::testing::Test {};

TEST_F(ModuloTest, BasicModulo) {
    VMTestHarness h;
    ASSERT_EQ(h.run("10 % 3;"), InterpretResult::OK);
    EXPECT_NEAR(as<Number>(h.lastResult()), 1.0, 1e-9);
}

TEST_F(ModuloTest, FloatModulo) {
    VMTestHarness h;
    ASSERT_EQ(h.run("7.5 % 2.5;"), InterpretResult::OK);
    EXPECT_NEAR(as<Number>(h.lastResult()), 0.0, 1e-9);
}

TEST_F(ModuloTest, NegativeDividend_FloorSemantics) {
    VMTestHarness h;
    ASSERT_EQ(h.run("-7 % 3;"), InterpretResult::OK);
    EXPECT_NEAR(as<Number>(h.lastResult()), 2.0, 1e-9);
}

TEST_F(ModuloTest, NegativeDivisor_FloorSemantics) {
    VMTestHarness h;
    ASSERT_EQ(h.run("7 % -3;"), InterpretResult::OK);
    EXPECT_NEAR(as<Number>(h.lastResult()), -2.0, 1e-9);
}

TEST_F(ModuloTest, ZeroDividend) {
    VMTestHarness h;
    ASSERT_EQ(h.run("0 % 5;"), InterpretResult::OK);
    EXPECT_NEAR(as<Number>(h.lastResult()), 0.0, 1e-9);
}
