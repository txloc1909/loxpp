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
#include "container_objects.h"
#include <gtest/gtest.h>
#include <cmath>
#include <cstdlib>
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
    // Generate 65536 distinct numeric literal expression statements.
    // The 65536th constant exhausts the pool (capacity: 65535) and must produce
    // a compile error — not a crash or out-of-bounds exception.
    std::string source;
    for (int i = 0; i < 65536; ++i)
        source += std::to_string(i) + ".5;\n"; // .5 avoids integer dedup
    VMTestHarness h;
    EXPECT_EQ(h.run(source), InterpretResult::COMPILE_ERROR);
}

TEST_F(CompileErrorTest, MoreThan255ConstantsSucceed) {
    // Programs with more than 255 distinct constants must compile and run.
    std::string source;
    for (int i = 0; i < 300; ++i)
        source +=
            "var v" + std::to_string(i) + " = " + std::to_string(i) + ".5;\n";
    VMTestHarness h;
    EXPECT_EQ(h.run(source), InterpretResult::OK);
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

// ===========================================================================
// Stack overflow guard tests
// ===========================================================================

class StackOverflowTest : public ::testing::Test {};

// Deep recursion that exceeds STACK_MAX should produce RUNTIME_ERROR, not
// crash. down(n) declares 20 local variables per frame (plus the callee slot
// and the argument slot, 22 slots/frame). This test isolates VM::push's
// STACK_MAX guard from the frame-count guard below only as long as one
// frame's slot cost is greater than STACK_MAX / FRAMES_MAX (16384 / 1024 =
// 16 slots): 22 > 16, so the value-stack limit binds first. If a future
// change to either constant raises that ratio to 22 or above, the
// frame-count guard fires first and this test stops testing STACK_MAX.
// n=900 exceeds the 16384-slot limit (900 * 22 = 19800) while its frame
// count (902) stays well under FRAMES_MAX (1024).
TEST_F(StackOverflowTest, DeepRecursionExceedsStackMax_RuntimeError) {
    VMTestHarness h;
    std::string src =
        "fun down(n) {"
        "  var a = 1; var b = 2; var c = 3; var d = 4; var e = 5;"
        "  var g = 6; var h = 7; var i = 8; var j = 9; var k = 10;"
        "  var l = 11; var m = 12; var o = 13; var p = 14; var q = 15;"
        "  var r = 16; var s = 17; var t = 18; var u = 19; var v = 20;"
        "  if (n == 0) return a+b+c+d+e+g+h+i+j+k+l+m+o+p+q+r+s+t+u+v;"
        "  return down(n - 1);"
        "}"
        "down(900);";
    EXPECT_EQ(h.run(src), InterpretResult::RUNTIME_ERROR);
    // Stack must be clean after a runtime error.
    EXPECT_EQ(h.stackDepth(), 0);
}

// Same 22-slot-per-frame shape as DeepRecursionExceedsStackMax_RuntimeError
// above, but at a depth (700) chosen to overflow the *old* STACK_MAX (2048
// slots, about 93 frames) while staying well under today's FRAMES_MAX
// ceiling (702 of 1024 frames). This shape overflows today's STACK_MAX at
// depth 744 (16384 slots), so depth 700 (15400 slots) leaves only 43 frames
// of room on the value-stack side, not "well under" -- a later change that
// adds even a couple of slots to this frame shape can fail this test for
// the STACK_MAX reason and not say so. Without this test at all, nothing in
// the suite fails if a future change reverts the STACK_MAX raise alone:
// every other fat-frame case here targets the failing side of that raise,
// and the thin-frame success tests below use too few slots per frame to
// notice it.
TEST_F(StackOverflowTest,
       DeepFatFrameRecursionPastOldStackMaxCeiling_Succeeds) {
    VMTestHarness h;
    std::string src =
        "fun down(n) {"
        "  var a = 1; var b = 2; var c = 3; var d = 4; var e = 5;"
        "  var g = 6; var h = 7; var i = 8; var j = 9; var k = 10;"
        "  var l = 11; var m = 12; var o = 13; var p = 14; var q = 15;"
        "  var r = 16; var s = 17; var t = 18; var u = 19; var v = 20;"
        "  if (n == 0) return a+b+c+d+e+g+h+i+j+k+l+m+o+p+q+r+s+t+u+v;"
        "  return down(n - 1);"
        "}"
        "down(700);";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.lastResult(), from<Number>(210.0));
    EXPECT_EQ(h.stackDepth(), 0);
}

// Recursion at a safe depth (down(50)) must still succeed with correct
// result. The guard must not reject a legitimate deep call chain. down(50)
// returns the sum of its 20 local variables: 1+2+...+20 = 210.
TEST_F(StackOverflowTest, SafeDepthRecursion_Succeeds) {
    VMTestHarness h;
    std::string src =
        "fun down(n) {"
        "  var a = 1; var b = 2; var c = 3; var d = 4; var e = 5;"
        "  var g = 6; var h = 7; var i = 8; var j = 9; var k = 10;"
        "  var l = 11; var m = 12; var o = 13; var p = 14; var q = 15;"
        "  var r = 16; var s = 17; var t = 18; var u = 19; var v = 20;"
        "  if (n == 0) return a+b+c+d+e+g+h+i+j+k+l+m+o+p+q+r+s+t+u+v;"
        "  return down(n - 1);"
        "}"
        "down(50);";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.lastResult(), from<Number>(210.0));
    EXPECT_EQ(h.stackDepth(), 0);
}

// down(n) here declares no locals, so its per-frame stack usage (2 slots:
// the callee and the argument) never comes close to STACK_MAX; only
// FRAMES_MAX bounds how deep it can go. 500 nested calls overflowed the old
// 256-frame ceiling; this proves the new ceiling actually moved, not just
// that some guard still fires eventually.
TEST_F(StackOverflowTest, RecursionPastOldFramesMaxCeiling_Succeeds) {
    VMTestHarness h;
    std::string src = "fun down(n) {"
                      "  if (n == 0) return 0;"
                      "  return down(n - 1);"
                      "}"
                      "down(500);";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.lastResult(), from<Number>(0.0));
    EXPECT_EQ(h.stackDepth(), 0);
}

// The script's own top-level call already occupies one of FRAMES_MAX's
// CallFrame slots before down() is ever called (VM::interpret() pushes it
// as frame 0), so a chain of N nested down() calls uses N+2 total frames.
// N=1022 is the deepest chain that still fits under FRAMES_MAX=1024;
// N=1023 must overflow it cleanly.
TEST_F(StackOverflowTest, DeepestFramesMaxRecursion_Succeeds) {
    VMTestHarness h;
    std::string src = "fun down(n) {"
                      "  if (n == 0) return 0;"
                      "  return down(n - 1);"
                      "}"
                      "down(1022);";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.lastResult(), from<Number>(0.0));
    EXPECT_EQ(h.stackDepth(), 0);
}

TEST_F(StackOverflowTest, DeepRecursionExceedsFramesMax_RuntimeError) {
    VMTestHarness h;
    std::string src = "fun down(n) {"
                      "  if (n == 0) return 0;"
                      "  return down(n - 1);"
                      "}"
                      "down(1023);";
    EXPECT_EQ(h.run(src), InterpretResult::RUNTIME_ERROR);
    // Stack must be clean after a runtime error, same as the STACK_MAX path.
    EXPECT_EQ(h.stackDepth(), 0);
}

// ===========================================================================
// Catchable StackOverflowError (spec/04-semantics.md's Runtime Errors table)
// ===========================================================================
//
// With no handler active, the two tests above are unaffected: recursion still
// runs all the way to FRAMES_MAX/STACK_MAX before failing, unchanged from
// before VM::STACK_OVERFLOW_FRAME_RESERVE/STACK_OVERFLOW_STACK_RESERVE
// existed. The tests below cover the case a handler IS active.

// Unbounded recursion under a try/catch must be caught as a StackOverflowError
// Error value, not fall through to the fatal path, on the frame-count guard.
TEST_F(StackOverflowTest, CatchableFramesOverflow_CaughtWithCorrectKind) {
    VMTestHarness h;
    std::string src = "fun f(n) { return f(n + 1); }"
                      "var kind; var msg;"
                      "try {"
                      "  f(0);"
                      "} catch (e) {"
                      "  kind = e.kind;"
                      "  msg = e.message;"
                      "}";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("kind"), "StackOverflowError");
    EXPECT_EQ(h.getGlobalStr("msg"), "Stack overflow.");
    EXPECT_EQ(h.stackDepth(), 0);
    EXPECT_EQ(h.handlerStackDepth(), 0);
}

// Same fault, but reached through the value-stack guard (push()) instead of
// the frame-count guard, via the same fat-frame shape the STACK_MAX test
// above uses. Proves both overflow sites, not just VM::call()'s, are wired.
TEST_F(StackOverflowTest,
       CatchableStackOverflow_FatFrame_CaughtWithCorrectKind) {
    VMTestHarness h;
    std::string src =
        "fun down(n) {"
        "  var a = 1; var b = 2; var c = 3; var d = 4; var e = 5;"
        "  var g = 6; var h = 7; var i = 8; var j = 9; var k = 10;"
        "  var l = 11; var m = 12; var o = 13; var p = 14; var q = 15;"
        "  var r = 16; var s = 17; var t = 18; var u = 19; var v = 20;"
        "  if (n == 0) return a+b+c+d+e+g+h+i+j+k+l+m+o+p+q+r+s+t+u+v;"
        "  return down(n - 1);"
        "}"
        "var kind;"
        "try {"
        "  down(2000);"
        "} catch (err) {"
        "  kind = err.kind;"
        "}";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("kind"), "StackOverflowError");
    EXPECT_EQ(h.stackDepth(), 0);
    EXPECT_EQ(h.handlerStackDepth(), 0);
}

// Catchability must not depend on how deep the try/catch itself was opened.
// The frame-count guard must test `>=`, not `==`, against FRAMES_MAX: the
// frame count only increases, so a try opened at or past that threshold must
// still see the guard fire on the very next call, not require passing
// through an exact value it may already be beyond. deep() recurses to
// exactly one frame short of FRAMES_MAX before opening the try, so boom()'s
// very first call already sees a frame count at the threshold.
TEST_F(StackOverflowTest,
       CatchableFramesOverflow_TryOpenedPastReserveThreshold) {
    VMTestHarness h;
    const int thresholdDepth = VM::FRAMES_MAX - 3;
    std::string src = "fun boom(n) { return boom(n + 1); }"
                      "fun deep(n) {"
                      "  if (n == 0) {"
                      "    try { boom(0); } catch (e) { kind = e.kind; }"
                      "    return 0;"
                      "  }"
                      "  return deep(n - 1);"
                      "}"
                      "var kind;"
                      "deep(" +
                      std::to_string(thresholdDepth) + ");";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("kind"), "StackOverflowError");
    EXPECT_EQ(h.stackDepth(), 0);
    EXPECT_EQ(h.handlerStackDepth(), 0);
}

// A pending defer must be catchable even on the frame that first crossed the
// value-stack threshold: handleThrow()'s unwind must reclaim each discarded
// frame's own stack window (see handleThrow()'s comment) before running its
// defer, so the reserve lasts the whole unwind instead of being exhausted by
// the second or third frame's defer alone. Same shape as
// CatchableStackOverflow_FatFrame_CaughtWithCorrectKind above, plus one
// `defer` per frame.
TEST_F(StackOverflowTest,
       CatchableStackOverflow_FatFrameWithDefer_CaughtWithCorrectKind) {
    VMTestHarness h;
    std::string src =
        "fun record(n) { return n; }"
        "fun down(n) {"
        "  var a = 1; var b = 2; var c = 3; var d = 4; var e = 5;"
        "  var g = 6; var h = 7; var i = 8; var j = 9; var k = 10;"
        "  var l = 11; var m = 12; var o = 13; var p = 14; var q = 15;"
        "  var r = 16; var s = 17; var t = 18; var u = 19; var v = 20;"
        "  defer record(n);"
        "  down(n - 1);"
        "}"
        "var kind;"
        "try {"
        "  down(2000);"
        "} catch (err) {"
        "  kind = err.kind;"
        "}";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("kind"), "StackOverflowError");
    EXPECT_EQ(h.stackDepth(), 0);
    EXPECT_EQ(h.handlerStackDepth(), 0);
}

// A handler-free deep recursion must still reach the full FRAMES_MAX ceiling
// — the reserve must not shrink the usable depth when nothing will catch the
// fault. Regression guard for the reserve added alongside the checks above:
// with no handler active, VM::call()'s early check must never fire, so this
// stays exactly the DeepestFramesMaxRecursion_Succeeds/
// DeepRecursionExceedsFramesMax_RuntimeError pair's own boundary (1022 ok,
// 1023 fatal), not FRAMES_MAX - STACK_OVERFLOW_FRAME_RESERVE.
TEST_F(StackOverflowTest, NoHandler_ReserveDoesNotShrinkUsableDepth) {
    VMTestHarness h;
    std::string src =
        "fun down(n) {"
        "  if (n == 0) return 0;"
        "  return down(n - 1);"
        "}"
        "down(" +
        std::to_string(VM::FRAMES_MAX - VM::STACK_OVERFLOW_FRAME_RESERVE) +
        ");";
    ASSERT_EQ(h.run(src), InterpretResult::OK)
        << "a depth inside the reserve window must still succeed when no "
           "handler is active to use that reserve";
}

// Every frame unwound on the way to the handler must run its own pending
// defer (spec/04-semantics.md's defer Statement, step 4) — not just the
// handful nearest the overflow that fit without the reserve. Each recursion
// level's defer appends its own depth to a list; the list, once caught, must
// hold every depth from the deepest call down to (but not including) the
// frame that installed the handler, in unwind order (deepest first).
//
// f's body calls f(n + 1) as a bare statement, not `return f(n + 1);`: an
// explicit-value return would run this frame's own RUN_DEFERS (and so
// record(n)) before evaluating the return expression, since
// Compiler::returnStatement() emits RUN_DEFERS ahead of the returned
// expression rather than after it — every existing defer example returns
// bare (`return;`) or falls off the end, so this ordering defect has no
// covering test yet (tracked separately; not this node's own mechanism).
// The bare-statement shape here reaches only the fall-off-the-end path,
// which is unaffected, so this test isolates handleThrow()'s own unwind-time
// draining — the mechanism this node actually adds — from that defect.
TEST_F(StackOverflowTest,
       CatchableFramesOverflow_DrainsEveryUnwoundFramesDefer) {
    VMTestHarness h;
    std::string src = "var ran = [];"
                      "fun record(n) { ran.append(n); }"
                      "fun f(n) {"
                      "  defer record(n);"
                      "  f(n + 1);"
                      "}"
                      "var kind;"
                      "try {"
                      "  f(0);"
                      "} catch (e) {"
                      "  kind = e.kind;"
                      "}";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("kind"), "StackOverflowError");
    auto ranVal = h.getGlobal("ran");
    ASSERT_TRUE(ranVal.has_value());
    ASSERT_TRUE(isList(*ranVal));
    ObjList* ran = asObjList(as<Obj*>(*ranVal));
    // The deepest frame (whichever one first hit the reserve boundary) drains
    // first, so its recorded depth is the list's first (highest) entry.
    ASSERT_FALSE(ran->elements.empty());
    for (std::size_t idx = 1; idx < ran->elements.size(); ++idx) {
        EXPECT_EQ(as<Number>(ran->elements[idx - 1]) - 1,
                  as<Number>(ran->elements[idx]))
            << "defers must run in LIFO unwind order (deepest frame first), "
               "with no frame's defer skipped, at list index "
            << idx;
    }
    EXPECT_EQ(h.stackDepth(), 0);
    EXPECT_EQ(h.handlerStackDepth(), 0);
}

// The StackOverflowError object itself must survive a collection that runs
// during its own unwind. Each unwound frame's defer here allocates a fresh
// string, and LOXPP_STRESS_GC=1 (set for VMTestHarness's own VM, restored
// after) forces a collection on every one of those allocations, so one runs
// between the Error's construction and the moment handleThrow() finally
// pushes it for the catch block to read. An Error left unrooted across that
// window is reclaimed and replaced by whatever allocation runs next, so the
// catch block reads freed memory instead of the StackOverflowError.
TEST_F(StackOverflowTest,
       CatchableFramesOverflow_AllocatingDeferSurvivesCollection) {
    const char* prevStressGC = std::getenv("LOXPP_STRESS_GC");
    std::string prevStressGCValue = prevStressGC ? prevStressGC : "";
    ::setenv("LOXPP_STRESS_GC", "1", 1);
    // MemoryManager reads this once at construction (see its own comment),
    // so it must be set before VMTestHarness's VM member exists, and it is
    // safe to restore right after — no VM constructed later in this test
    // reads it again.
    VMTestHarness h;
    if (prevStressGC) {
        ::setenv("LOXPP_STRESS_GC", prevStressGCValue.c_str(), 1);
    } else {
        ::unsetenv("LOXPP_STRESS_GC");
    }
    std::string src = "fun record(n) { return \"padded-\" + str(n); }"
                      "fun f(n) {"
                      "  defer record(n);"
                      "  f(n + 1);"
                      "}"
                      "var kind; var msg;"
                      "try {"
                      "  f(0);"
                      "} catch (e) {"
                      "  kind = e.kind;"
                      "  msg = e.message;"
                      "}";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("kind"), "StackOverflowError");
    EXPECT_EQ(h.getGlobalStr("msg"), "Stack overflow.");
    EXPECT_EQ(h.stackDepth(), 0);
    EXPECT_EQ(h.handlerStackDepth(), 0);
}

// A deferred call that itself recurses far enough to outrun the reserve must
// not hang or corrupt VM state. This directly exercises the "prove a new
// check can fail" hazard the frame reserve exists to bound: with the guard
// in VM::call()/push() removed (m_unwindingStackOverflow ignored), this
// program does not hang — it crashes. Every one of `f`'s ~1000 unwound
// frames hands its own recorded `boom(n)` to handleThrow()'s unwind loop;
// boom's own 500-deep recursion reaches the reserve boundary again while the
// ORIGINAL StackOverflowError's handleThrow() call is still on the C++ call
// stack, and an unguarded second catch attempt there reenters handleThrow()
// through genuine C++ recursion (run() -> runPendingDefers() ->
// handleThrow() -> raiseThrowableError() -> a new run()) once per remaining
// frame — verified directly: reverting m_unwindingStackOverflow's guard at
// both sites reproduces a real AddressSanitizer stack-overflow abort inside
// this exact test, not a graceful Lox++-level fault. The fix makes the
// second overflow "stay fatal" (this row's own original design note) rather
// than try to catch it again: the outer try/catch below never runs its
// catchBlock, the whole program halts uncaught, and the check here is that
// halting is all it does — no crash, no hang, and a fully torn-down VM.
TEST_F(StackOverflowTest, SecondOverflowDuringUnwindDoesNotHangOrCorrupt) {
    VMTestHarness h;
    // f(n + 1) is a bare statement, not a return expression — see the
    // comment on CatchableFramesOverflow_DrainsEveryUnwoundFramesDefer for
    // why: this keeps every frame's defer pending until handleThrow's own
    // unwind, which is what this test means to stress.
    std::string src = "fun deepcall(n) {"
                      "  if (n == 0) return 0;"
                      "  return deepcall(n - 1);"
                      "}"
                      "fun boom(n) { deepcall(500); }"
                      "fun f(n) {"
                      "  defer boom(n);"
                      "  f(n + 1);"
                      "}"
                      "try {"
                      "  f(0);"
                      "} catch (e) {"
                      "}";
    // The second, deeper overflow (inside boom's own deepcall(500)) reaches
    // the true FRAMES_MAX ceiling before it can be drained to completion, so
    // this resolves as a hard, uncaught error — not the outer try/catch
    // catching it — matching the "stays fatal" design this reserve accepts.
    EXPECT_EQ(h.run(src), InterpretResult::RUNTIME_ERROR);
    // Whatever path the second overflow took, the VM must land in a clean,
    // fully-unwound state — never a stray handler, frame, or stack value.
    EXPECT_EQ(h.stackDepth(), 0);
    EXPECT_EQ(h.handlerStackDepth(), 0);
}

// ===========================================================================
// handleThrow() must root thrownValue for its whole unwind, not just
// the ObjError raiseThrowableError() builds. A plain `throw` of a heap value
// takes the same handleThrow() unwind as a StackOverflowError, so it needs
// the same protection: a defer in the unwound frame runs arbitrary Lox++
// code and can allocate, forcing a collection (LOXPP_STRESS_GC=1) while
// thrownValue is off the value stack. Two shapes: a named local (the only
// reference is the frame slot that gets reclaimed) and a temporary (never
// named at all).
// ===========================================================================

TEST_F(StackOverflowTest,
       CatchableThrow_NamedLocalSurvivesCollectionDuringUnwind) {
    const char* prevStressGC = std::getenv("LOXPP_STRESS_GC");
    std::string prevStressGCValue = prevStressGC ? prevStressGC : "";
    ::setenv("LOXPP_STRESS_GC", "1", 1);
    VMTestHarness h;
    if (prevStressGC) {
        ::setenv("LOXPP_STRESS_GC", prevStressGCValue.c_str(), 1);
    } else {
        ::unsetenv("LOXPP_STRESS_GC");
    }
    std::string src = "fun alloc(n) {"
                      "  var s = \"\"; var i = 0;"
                      "  while (i < 30) { s = s + \"padpadpadpad\" + str(n);"
                      "                   i = i + 1; }"
                      "  return s;"
                      "}"
                      "fun g(n) {"
                      "  var payload = \"payload-\" + str(n);"
                      "  defer alloc(n);"
                      "  throw payload;"
                      "}"
                      "var caught;"
                      "try { g(7); } catch (e) { caught = e; }";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("caught"), "payload-7");
    EXPECT_EQ(h.stackDepth(), 0);
    EXPECT_EQ(h.handlerStackDepth(), 0);
}

TEST_F(StackOverflowTest,
       CatchableThrow_TemporarySurvivesCollectionDuringUnwind) {
    const char* prevStressGC = std::getenv("LOXPP_STRESS_GC");
    std::string prevStressGCValue = prevStressGC ? prevStressGC : "";
    ::setenv("LOXPP_STRESS_GC", "1", 1);
    VMTestHarness h;
    if (prevStressGC) {
        ::setenv("LOXPP_STRESS_GC", prevStressGCValue.c_str(), 1);
    } else {
        ::unsetenv("LOXPP_STRESS_GC");
    }
    std::string src = "fun alloc(n) {"
                      "  var s = \"\"; var i = 0;"
                      "  while (i < 30) { s = s + \"padpadpadpad\" + str(n);"
                      "                   i = i + 1; }"
                      "  return s;"
                      "}"
                      "fun g(n) {"
                      "  defer alloc(n);"
                      "  throw \"payload-\" + str(n);"
                      "}"
                      "var caught;"
                      "try { g(7); } catch (e) { caught = e; }";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("caught"), "payload-7");
    EXPECT_EQ(h.stackDepth(), 0);
    EXPECT_EQ(h.handlerStackDepth(), 0);
}

// ===========================================================================
// An open try/catch must never cost even one value-stack slot.
// NoHandler_ReserveDoesNotShrinkUsableDepth covers only the no-handler
// half; these cover the handler-active half at both overflow sites: the
// greatest depth that succeeds with no try must also succeed inside a try,
// and one step deeper must still fail with no try.
// ===========================================================================

// Frame-count site. DeepRecursionExceedsFramesMax_RuntimeError already
// covers "1023 fails with no try"; this covers "1022 succeeds inside a try"
// — the half that was still missing at the sibling (value-stack) site.
TEST_F(StackOverflowTest,
       CatchableFramesOverflow_TryOpenDoesNotShrinkUsableDepth) {
    VMTestHarness h;
    std::string src = "fun down(n) {"
                      "  if (n == 0) return 0;"
                      "  return down(n - 1);"
                      "}"
                      "var ok = false; var caught;"
                      "try { down(1022); ok = true; }"
                      "catch (e) { caught = e.kind; }";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    expect_global_bool(h, "ok", true);
    expect_global_nil(h, "caught");
    EXPECT_EQ(h.stackDepth(), 0);
    EXPECT_EQ(h.handlerStackDepth(), 0);
}

// Value-stack site. Same 20-local-per-frame shape as the fat-frame tests
// above, recursed to depth 743, plus N "pad" locals declared in the
// enclosing scope so the peak lands exactly at STACK_MAX slots. Measured:
// pad=13 succeeds with no try but was wrongly caught as StackOverflowError
// inside a try (the soft threshold's own one-slot gap); pad=14 fails both
// ways. makePadLocals(n) generates "var p0=0; ... var
// p(n-1)=n-1;" so the exact boundary count does not need to be hand-typed.
namespace {
std::string makePadLocals(int count) {
    std::string out;
    for (int i = 0; i < count; ++i) {
        out += "var p" + std::to_string(i) + " = " + std::to_string(i) + "; ";
    }
    return out;
}

std::string fatFrameDownFn() {
    return "fun down(n) {"
           "  var a = 1; var b = 2; var c = 3; var d = 4; var e = 5;"
           "  var g = 6; var h = 7; var i = 8; var j = 9; var k = 10;"
           "  var l = 11; var m = 12; var o = 13; var p = 14; var q = 15;"
           "  var r = 16; var s = 17; var t = 18; var u = 19; var v = 20;"
           "  if (n == 0) return 0;"
           "  return down(n - 1);"
           "}";
}
} // namespace

TEST_F(StackOverflowTest,
       CatchableStackOverflow_FatFrame_TryOpenDoesNotShrinkUsableDepth) {
    VMTestHarness h;
    std::string src = fatFrameDownFn() + "var ok = false; var caught;" +
                      "try { " + makePadLocals(13) +
                      "down(743); ok = true; }"
                      "catch (e) { caught = e.kind; }";
    ASSERT_EQ(h.run(src), InterpretResult::OK);
    expect_global_bool(h, "ok", true);
    expect_global_nil(h, "caught");
    EXPECT_EQ(h.stackDepth(), 0);
    EXPECT_EQ(h.handlerStackDepth(), 0);
}

TEST_F(StackOverflowTest,
       CatchableStackOverflow_FatFrame_OneMoreSlot_RuntimeErrorWithNoTry) {
    VMTestHarness h;
    std::string src =
        fatFrameDownFn() + "{ " + makePadLocals(14) + "down(743); }";
    EXPECT_EQ(h.run(src), InterpretResult::RUNTIME_ERROR);
    EXPECT_EQ(h.stackDepth(), 0);
}

// push() tests a moving ceiling (STACK_MAX, or STACK_MAX plus the reserve
// while unwinding a StackOverflowError), so it must use >=, not ==: a
// pointer already past STACK_MAX when the flag clears would never hit an
// exact match again. No Lox program can reach that state (handleThrow()
// resets stackTop first), so drive push() directly through the VMTestAccess
// seam instead.
struct VMTestAccess {
    static void setStackTop(VM& vm, int depth) {
        vm.m_rt.stackTop = vm.m_rt.stack + depth;
    }
    static void setUnwinding(VM& vm, bool v) {
        vm.m_rt.m_unwindingStackOverflow = v;
    }
    static void push(VM& vm, Value v) { vm.m_rt.push(v); }
    static bool overflowFlag(const VM& vm) { return vm.m_rt.m_stackOverflow; }
    static int depth(const VM& vm) {
        return static_cast<int>(vm.m_rt.stackTop - vm.m_rt.stack);
    }
};

TEST_F(StackOverflowTest, PushPastCeiling_SetsOverflow) {
    // At the ceiling: both == and >= trip. Guards the boundary itself.
    {
        VM vm;
        VMTestAccess::setUnwinding(vm, false);
        VMTestAccess::setStackTop(vm, VM::STACK_MAX);
        VMTestAccess::push(vm, from<Number>(1.0));
        EXPECT_TRUE(VMTestAccess::overflowFlag(vm));
        EXPECT_EQ(VMTestAccess::depth(vm), VM::STACK_MAX);
    }
    // One past the ceiling with the reserve cleared: only >= trips. This is
    // the #313 hazard — the ceiling just moved down from STACK_MAX plus the
    // reserve, leaving stackTop above it.
    {
        VM vm;
        VMTestAccess::setUnwinding(vm, false);
        VMTestAccess::setStackTop(vm, VM::STACK_MAX + 1);
        VMTestAccess::push(vm, from<Number>(1.0));
        EXPECT_TRUE(VMTestAccess::overflowFlag(vm));
        EXPECT_EQ(VMTestAccess::depth(vm), VM::STACK_MAX + 1);
    }
    // At the physical end with the reserve cleared: only >= trips.
    {
        VM vm;
        VMTestAccess::setUnwinding(vm, false);
        VMTestAccess::setStackTop(vm, VM::STACK_MAX +
                                          VM::STACK_OVERFLOW_STACK_RESERVE);
        VMTestAccess::push(vm, from<Number>(1.0));
        EXPECT_TRUE(VMTestAccess::overflowFlag(vm));
        EXPECT_EQ(VMTestAccess::depth(vm),
                  VM::STACK_MAX + VM::STACK_OVERFLOW_STACK_RESERVE);
    }
}
