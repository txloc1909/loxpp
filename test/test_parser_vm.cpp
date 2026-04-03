#include "test_harness.h"
#include <gtest/gtest.h>

class ParserVMTest : public ::testing::Test {};

// Primary helper: eval expression, compare stringify output to expected string.
// This matches the "// expect: X" format used in the craftinginterpreters test suite.
static void expect_result(const std::string& expr, const std::string& expected) {
    EXPECT_EQ(eval_expr_str(expr), expected) << "  expr: " << expr;
}

// Helper for expressions expected to produce a runtime error.
static void expect_runtime_error(const std::string& expr) {
    EXPECT_THROW(eval_expr(expr), std::runtime_error) << "  expr: " << expr;
}

// ---------------------------------------------------------------------------
// test/expressions/evaluate.lox
// ---------------------------------------------------------------------------
TEST_F(ParserVMTest, Expressions_Evaluate) {
    expect_result("(5 - (3 - 1)) + -1", "2");
}

// ---------------------------------------------------------------------------
// test/bool/not.lox
// ---------------------------------------------------------------------------
TEST_F(ParserVMTest, Bool_Not) {
    expect_result("!true",  "false");
    expect_result("!false", "true");
    expect_result("!!true", "true");
}

// ---------------------------------------------------------------------------
// test/bool/equality.lox
// ---------------------------------------------------------------------------
TEST_F(ParserVMTest, Bool_Equality) {
    expect_result("true == true",    "true");
    expect_result("true == false",   "false");
    expect_result("false == true",   "false");
    expect_result("false == false",  "true");

    // Not equal to other types.
    expect_result("true == 1",       "false");
    expect_result("false == 0",      "false");
    expect_result("true == \"true\"",  "false");
    expect_result("false == \"false\"","false");
    expect_result("false == \"\"",     "false");

    expect_result("true != true",    "false");
    expect_result("true != false",   "true");
    expect_result("false != true",   "true");
    expect_result("false != false",  "false");

    expect_result("true != 1",       "true");
    expect_result("false != 0",      "true");
    expect_result("true != \"true\"",  "true");
    expect_result("false != \"false\"","true");
    expect_result("false != \"\"",     "true");
}

// ---------------------------------------------------------------------------
// test/nil/literal.lox
// ---------------------------------------------------------------------------
TEST_F(ParserVMTest, Nil_Literal) {
    expect_result("nil", "nil");
}

// ---------------------------------------------------------------------------
// test/number/literals.lox
// ---------------------------------------------------------------------------
TEST_F(ParserVMTest, Number_Literals) {
    expect_result("123",    "123");
    expect_result("987654", "987654");
    expect_result("0",      "0");
    expect_result("-0",     "-0");
    expect_result("123.456","123.456");
    expect_result("-0.001", "-0.001");
}

// ---------------------------------------------------------------------------
// test/string/literals.lox
// ---------------------------------------------------------------------------
TEST_F(ParserVMTest, String_Literals) {
    expect_result("\"(\" + \"\" + \")\"", "()");
    expect_result("\"a string\"",         "a string");
    expect_result("\"A~\xc2\xb6\xc3\x9e\xe0\xa5\x90\xe0\xae\x83\"",
                  "A~\xc2\xb6\xc3\x9e\xe0\xa5\x90\xe0\xae\x83"); // A~¶Þॐஃ
}

// ---------------------------------------------------------------------------
// test/operator/add.lox
// ---------------------------------------------------------------------------
TEST_F(ParserVMTest, Operator_Add) {
    expect_result("123 + 456",        "579");
    expect_result("\"str\" + \"ing\"","string");
}

// ---------------------------------------------------------------------------
// test/operator/subtract.lox
// ---------------------------------------------------------------------------
TEST_F(ParserVMTest, Operator_Subtract) {
    expect_result("4 - 3",     "1");
    expect_result("1.2 - 1.2", "0");
}

// ---------------------------------------------------------------------------
// test/operator/multiply.lox
// ---------------------------------------------------------------------------
TEST_F(ParserVMTest, Operator_Multiply) {
    expect_result("5 * 3",        "15");
    expect_result("12.34 * 0.3",  "3.702");
}

// ---------------------------------------------------------------------------
// test/operator/divide.lox
// ---------------------------------------------------------------------------
TEST_F(ParserVMTest, Operator_Divide) {
    expect_result("8 / 2",         "4");
    expect_result("12.34 / 12.34", "1");
}

// ---------------------------------------------------------------------------
// test/operator/negate.lox
// ---------------------------------------------------------------------------
TEST_F(ParserVMTest, Operator_Negate) {
    expect_result("-(3)",   "-3");
    expect_result("--(3)",  "3");
    expect_result("---(3)", "-3");
}

// ---------------------------------------------------------------------------
// test/operator/not.lox  (fun foo() case skipped — functions not yet implemented)
// ---------------------------------------------------------------------------
TEST_F(ParserVMTest, Operator_Not) {
    expect_result("!true",  "false");
    expect_result("!false", "true");
    expect_result("!!true", "true");

    expect_result("!123", "false");
    expect_result("!0",   "false");

    expect_result("!nil", "true");

    expect_result("!\"\"", "false");
}

// ---------------------------------------------------------------------------
// test/operator/comparison.lox
// ---------------------------------------------------------------------------
TEST_F(ParserVMTest, Operator_Comparison) {
    expect_result("1 < 2", "true");
    expect_result("2 < 2", "false");
    expect_result("2 < 1", "false");

    expect_result("1 <= 2", "true");
    expect_result("2 <= 2", "true");
    expect_result("2 <= 1", "false");

    expect_result("1 > 2", "false");
    expect_result("2 > 2", "false");
    expect_result("2 > 1", "true");

    expect_result("1 >= 2", "false");
    expect_result("2 >= 2", "true");
    expect_result("2 >= 1", "true");

    // Zero and negative zero compare the same.
    expect_result("0 < -0",  "false");
    expect_result("-0 < 0",  "false");
    expect_result("0 > -0",  "false");
    expect_result("-0 > 0",  "false");
    expect_result("0 <= -0", "true");
    expect_result("-0 <= 0", "true");
    expect_result("0 >= -0", "true");
    expect_result("-0 >= 0", "true");
}

// ---------------------------------------------------------------------------
// test/operator/equals.lox
// ---------------------------------------------------------------------------
TEST_F(ParserVMTest, Operator_Equals) {
    expect_result("nil == nil",           "true");
    expect_result("true == true",         "true");
    expect_result("true == false",        "false");
    expect_result("1 == 1",               "true");
    expect_result("1 == 2",               "false");
    expect_result("\"str\" == \"str\"",   "true");
    expect_result("\"str\" == \"ing\"",   "false");
    expect_result("nil == false",         "false");
    expect_result("false == 0",           "false");
    expect_result("0 == \"0\"",           "false");
}

// ---------------------------------------------------------------------------
// test/operator/not_equals.lox
// ---------------------------------------------------------------------------
TEST_F(ParserVMTest, Operator_NotEquals) {
    expect_result("nil != nil",           "false");
    expect_result("true != true",         "false");
    expect_result("true != false",        "true");
    expect_result("1 != 1",               "false");
    expect_result("1 != 2",               "true");
    expect_result("\"str\" != \"str\"",   "false");
    expect_result("\"str\" != \"ing\"",   "true");
    expect_result("nil != false",         "true");
    expect_result("false != 0",           "true");
    expect_result("0 != \"0\"",           "true");
}

// ---------------------------------------------------------------------------
// Runtime error cases
// test/operator/add_bool_nil.lox, add_bool_num.lox, add_bool_string.lox,
//   add_nil_nil.lox, divide_nonnum_num.lox, greater_nonnum_num.lox,
//   negate_nonnum.lox
// ---------------------------------------------------------------------------
TEST_F(ParserVMTest, Operator_RuntimeErrors) {
    expect_runtime_error("true + nil");
    expect_runtime_error("true + 123");
    expect_runtime_error("true + \"s\"");
    expect_runtime_error("nil + nil");
    expect_runtime_error("\"1\" / 1");
    expect_runtime_error("\"1\" > 1");
    expect_runtime_error("-\"s\"");
}

// ---------------------------------------------------------------------------
// Disabled: require variable declarations (var) not yet implemented
// test/number/nan_equality.lox, test/string/multiline.lox
// ---------------------------------------------------------------------------
TEST_F(ParserVMTest, DISABLED_BooleanLogicAndOr) {
    // and/or require jump instructions (short-circuit eval) — not yet implemented
    expect_result("true and false", "false");
    expect_result("true or false",  "true");
}

