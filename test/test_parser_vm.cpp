#include "test_harness.h"
#include "vm.h"
#include <gtest/gtest.h>
#include <cmath>

class ParserVMTest : public ::testing::Test {};

// ---------------------------------------------------------------------------
// Typed assertion helpers
// ---------------------------------------------------------------------------

static void expect_num(const std::string& expr, double expected) {
    Value v = eval_expr(expr);
    ASSERT_TRUE(std::holds_alternative<Number>(v))
        << "  expr: " << expr << "\n  expected number, got different type";
    EXPECT_NEAR(std::get<Number>(v), expected, 1e-9) << "  expr: " << expr;
}

static void expect_bool(const std::string& expr, bool expected) {
    Value v = eval_expr(expr);
    ASSERT_TRUE(std::holds_alternative<bool>(v))
        << "  expr: " << expr << "\n  expected bool, got different type";
    EXPECT_EQ(std::get<bool>(v), expected) << "  expr: " << expr;
}

static void expect_nil(const std::string& expr) {
    Value v = eval_expr(expr);
    EXPECT_TRUE(std::holds_alternative<Nil>(v))
        << "  expr: " << expr << "\n  expected nil, got different type";
}

// String results: eval_expr_str keeps the VM alive so Obj* remains valid.
static void expect_str(const std::string& expr, const std::string& expected) {
    EXPECT_EQ(eval_expr_str(expr), expected) << "  expr: " << expr;
}

static void expect_runtime_error(const std::string& expr) {
    EXPECT_THROW(eval_expr(expr), std::runtime_error) << "  expr: " << expr;
}

// ===========================================================================
// Literals
// ===========================================================================

TEST_F(ParserVMTest, NumberLiterals) {
    expect_num("0", 0);
    expect_num("123", 123);
    expect_num("987654", 987654);
    expect_num("123.456", 123.456);
    expect_num("-0.001", -0.001);
    // Negative zero is numerically == 0 but must display as "-0".
    expect_str("-0", "-0");
}

TEST_F(ParserVMTest, BoolLiterals) {
    expect_bool("true", true);
    expect_bool("false", false);
}

TEST_F(ParserVMTest, NilLiteral) { expect_nil("nil"); }

TEST_F(ParserVMTest, StringLiterals) {
    expect_str("\"\"", "");
    expect_str("\"hello\"", "hello");
    expect_str("\"a string\"", "a string");
    // Non-ASCII (UTF-8 encoded): A~¶Þॐஃ
    expect_str("\"A~\xc2\xb6\xc3\x9e\xe0\xa5\x90\xe0\xae\x83\"",
               "A~\xc2\xb6\xc3\x9e\xe0\xa5\x90\xe0\xae\x83");
}

// ===========================================================================
// Arithmetic
// ===========================================================================

TEST_F(ParserVMTest, Addition) {
    expect_num("1 + 2", 3);
    expect_num("123 + 456", 579);
    expect_num("0.1 + 0.2", 0.3);
    expect_num("0 + 0", 0);
    expect_num("-1 + 1", 0);
    expect_num("-3 + -4", -7);
}

TEST_F(ParserVMTest, Subtraction) {
    expect_num("4 - 3", 1);
    expect_num("0 - 0", 0);
    expect_num("1.2 - 1.2", 0);
    expect_num("0 - 5", -5);
    expect_num("10 - 3 - 2", 5); // left-associative
}

TEST_F(ParserVMTest, Multiplication) {
    expect_num("5 * 3", 15);
    expect_num("0 * 999", 0);
    expect_num("1 * 1", 1);
    expect_num("-2 * 3", -6);
    expect_num("12.34 * 0.3", 3.702);
}

TEST_F(ParserVMTest, Division) {
    expect_num("8 / 2", 4);
    expect_num("1 / 1", 1);
    expect_num("12.34 / 12.34", 1);
    expect_num("1 / 4", 0.25);
    expect_num("10 / 4 / 2", 1.25); // left-associative: (10/4)/2
}

TEST_F(ParserVMTest, Negation) {
    expect_num("-(3)", -3);
    expect_num("--(3)", 3);
    expect_num("---(3)", -3);
    expect_num("-0", 0); // -0 == 0 numerically
    expect_num("-123.5", -123.5);
}

TEST_F(ParserVMTest, PrecedenceAndGrouping) {
    expect_num("2 + 3 * 4", 14); // * before +
    expect_num("(2 + 3) * 4", 20);
    expect_num("8 / 2 - 1", 3);
    expect_num("1 + 2 * 3 - 4", 3);
    expect_num("(5 - (3 - 1)) + -1", 2);
    expect_num("-(1 + 2)", -3);
    expect_num("-(-5)", 5);
}

// ===========================================================================
// String concatenation
// ===========================================================================

TEST_F(ParserVMTest, StringConcatenation) {
    expect_str("\"str\" + \"ing\"", "string");
    expect_str("\"\" + \"hello\"", "hello");
    expect_str("\"hello\" + \"\"", "hello");
    expect_str("\"\" + \"\"", "");
    expect_str("\"a\" + \"b\" + \"c\"", "abc"); // left-associative
    expect_str("\"(\" + \"\" + \")\"", "()");
    expect_str("\"foo\" + \"bar\" + \"baz\"", "foobarbaz");
}

// ===========================================================================
// Comparison operators  (<, <=, >, >=)
// ===========================================================================

TEST_F(ParserVMTest, LessThan) {
    expect_bool("1 < 2", true);
    expect_bool("2 < 2", false);
    expect_bool("2 < 1", false);
    expect_bool("-1 < 0", true);
    expect_bool("0 < -0", false); // -0 == 0
}

TEST_F(ParserVMTest, LessOrEqual) {
    expect_bool("1 <= 2", true);
    expect_bool("2 <= 2", true);
    expect_bool("2 <= 1", false);
    expect_bool("-0 <= 0", true); // -0 == 0
    expect_bool("0 <= -0", true);
}

TEST_F(ParserVMTest, GreaterThan) {
    expect_bool("2 > 1", true);
    expect_bool("2 > 2", false);
    expect_bool("1 > 2", false);
    expect_bool("0 > -1", true);
    expect_bool("0 > -0", false); // -0 == 0
}

TEST_F(ParserVMTest, GreaterOrEqual) {
    expect_bool("2 >= 1", true);
    expect_bool("2 >= 2", true);
    expect_bool("1 >= 2", false);
    expect_bool("-0 >= 0", true); // -0 == 0
    expect_bool("0 >= -0", true);
}

// ===========================================================================
// Equality operators  (==, !=)
// ===========================================================================

TEST_F(ParserVMTest, EqualityNumbers) {
    expect_bool("1 == 1", true);
    expect_bool("1 == 2", false);
    expect_bool("1 != 1", false);
    expect_bool("1 != 2", true);
    expect_bool("0 == -0", true); // IEEE 754: 0.0 == -0.0
}

TEST_F(ParserVMTest, EqualityBools) {
    expect_bool("true == true", true);
    expect_bool("true == false", false);
    expect_bool("false == false", true);
    expect_bool("true != false", true);
    expect_bool("false != false", false);
}

TEST_F(ParserVMTest, EqualityStrings) {
    expect_bool("\"str\" == \"str\"", true);
    expect_bool("\"str\" == \"ing\"", false);
    expect_bool("\"\" == \"\"", true);
    expect_bool("\"str\" != \"str\"", false);
    expect_bool("\"str\" != \"ing\"", true);
}

TEST_F(ParserVMTest, EqualityNil) {
    expect_bool("nil == nil", true);
    expect_bool("nil != nil", false);
}

TEST_F(ParserVMTest, EqualityCrossType) {
    // Values of different types are never equal.
    expect_bool("nil == false", false);
    expect_bool("nil == 0", false);
    expect_bool("nil == \"\"", false);
    expect_bool("false == 0", false);
    expect_bool("true == 1", false);
    expect_bool("0 == \"0\"", false);
    expect_bool("true == \"true\"", false);
    expect_bool("false == \"false\"", false);
    expect_bool("false == \"\"", false);
    // != mirrors ==
    expect_bool("nil != false", true);
    expect_bool("false != 0", true);
    expect_bool("0 != \"0\"", true);
}

// ===========================================================================
// Logical / Unary  (!, truthiness)
// ===========================================================================

TEST_F(ParserVMTest, LogicalNot) {
    expect_bool("!true", false);
    expect_bool("!false", true);
    expect_bool("!!true", true);
    expect_bool("!!false", false);
    expect_bool("!!!true", false);
}

TEST_F(ParserVMTest, Truthiness) {
    // Only nil and false are falsy; everything else is truthy.
    expect_bool("!nil", true);
    expect_bool("!false", true);
    expect_bool("!true", false);
    expect_bool("!0", false); // 0 is truthy
    expect_bool("!123", false);
    expect_bool("!\"\"", false); // empty string is truthy
    expect_bool("!\"x\"", false);
}

TEST_F(ParserVMTest, DISABLED_LogicalAndOr) {
    // and/or require jump instructions (short-circuit eval) — not yet
    // implemented.
    expect_bool("true and false", false);
    expect_bool("true and true", true);
    expect_bool("false and false", false);
    expect_bool("true or false", true);
    expect_bool("false or false", false);
    expect_bool("false or true", true);
}

// ===========================================================================
// Runtime errors — type mismatches
// ===========================================================================

TEST_F(ParserVMTest, RuntimeError_AddTypeMismatch) {
    expect_runtime_error("true + nil");
    expect_runtime_error("true + 123");
    expect_runtime_error("true + \"s\"");
    expect_runtime_error("nil + nil");
    expect_runtime_error("1 + nil");
    expect_runtime_error("1 + true");
}

TEST_F(ParserVMTest, RuntimeError_ArithmeticOnNonNumbers) {
    expect_runtime_error("\"1\" - 1");
    expect_runtime_error("\"1\" * 1");
    expect_runtime_error("\"1\" / 1");
    expect_runtime_error("nil - 1");
    expect_runtime_error("true * 2");
}

TEST_F(ParserVMTest, RuntimeError_ComparisonOnNonNumbers) {
    expect_runtime_error("\"1\" > 1");
    expect_runtime_error("\"1\" < 1");
    expect_runtime_error("\"1\" >= 1");
    expect_runtime_error("\"1\" <= 1");
    expect_runtime_error("nil > 0");
    expect_runtime_error("true < false");
}

TEST_F(ParserVMTest, RuntimeError_NegateNonNumber) {
    expect_runtime_error("-\"s\"");
    expect_runtime_error("-nil");
    expect_runtime_error("-true");
}

// ===========================================================================
// Statements
// ===========================================================================

TEST_F(ParserVMTest, PrintStatement_Number) {
    // print 1 + 2; should not crash and the VM should return OK.
    VM vm;
    EXPECT_EQ(vm.interpret("print 1 + 2;"), InterpretResult::OK);
}

TEST_F(ParserVMTest, PrintStatement_String) {
    VM vm;
    EXPECT_EQ(vm.interpret("print \"hello\";"), InterpretResult::OK);
}

TEST_F(ParserVMTest, ExpressionStatement_Discards) {
    // An expression statement evaluates and discards; the last POP sets
    // m_lastResult, so we can still inspect the discarded value via eval_expr.
    expect_num("3.14 * 2", 6.28);
}

TEST_F(ParserVMTest, MultiStatement_PrintThenExpr) {
    // Acceptance-criteria input: print 1 + 2; 3.14 * 2 * 3;
    VM vm;
    EXPECT_EQ(vm.interpret("print 1 + 2; 3.14 * 2 * 3;"), InterpretResult::OK);
}

TEST_F(ParserVMTest, MultiStatement_ExprThenPrint) {
    VM vm;
    EXPECT_EQ(vm.interpret("1 + 1; print 2 + 2;"), InterpretResult::OK);
}
