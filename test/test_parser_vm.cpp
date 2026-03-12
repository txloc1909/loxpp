#include "test_harness.h"
#include <gtest/gtest.h>
#include <cmath>

class ParserVMTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Helper for floating point comparison
static void expect_number_eq(const std::string& expr, double expected) {
    Value v = eval_expr(expr);
    ASSERT_TRUE(std::holds_alternative<Number>(v)) << "Result is not a number";
    EXPECT_NEAR(std::get<Number>(v), expected, 1e-9);
}

// Helper for bool
static void expect_bool_eq(const std::string& expr, bool expected) {
    Value v = eval_expr(expr);
    ASSERT_TRUE(std::holds_alternative<bool>(v)) << "Result is not a bool";
    EXPECT_EQ(std::get<bool>(v), expected);
}

// Helper for string
static void expect_string_eq(const std::string& expr, const std::string& expected) {
    Value v = eval_expr(expr);
    ASSERT_TRUE(std::holds_alternative<Obj*>(v)) << "Result is not an object";
    Obj* obj = std::get<Obj*>(v);
    ASSERT_TRUE(isObjType(obj, ObjType::STRING)) << "Result is not a string object";
    EXPECT_EQ(static_cast<ObjString*>(obj)->chars, expected);
}

// Helper for nil
static void expect_nil(const std::string& expr) {
    Value v = eval_expr(expr);
    ASSERT_TRUE(std::holds_alternative<Nil>(v)) << "Result is not nil";
}

TEST_F(ParserVMTest, Arithmetic) {
    expect_number_eq("1 + 2 * 3", 7);
    expect_number_eq("(1 + 2) * 3", 9);
    expect_number_eq("8 / 2 - 1", 3);
    expect_number_eq("5 - 2 - 1", 2);
    expect_number_eq("2.5 * 4", 10.0);
}

TEST_F(ParserVMTest, Comparison) {
    expect_bool_eq("3 > 2", true);
    expect_bool_eq("2 < 1", false);
    expect_bool_eq("2 == 2", true);
    expect_bool_eq("2 != 3", true);
    expect_bool_eq("1 >= 1", true);
    expect_bool_eq("1 <= 0", false);
}

TEST_F(ParserVMTest, BooleanLogic) {
    expect_bool_eq("true and false", false);
    expect_bool_eq("true or false", true);
    expect_bool_eq("!true", false);
    expect_bool_eq("!false", true);
    expect_bool_eq("!nil", true);
    expect_bool_eq("!0", false); // 0 is truthy in Lox
}

TEST_F(ParserVMTest, StringConcat) {
    expect_string_eq("\"foo\" + \"bar\"", "foobar");
    expect_string_eq("\"a\" + \"b\" + \"c\"", "abc");
}

TEST_F(ParserVMTest, NilLiteral) {
    expect_nil("nil");
}

TEST_F(ParserVMTest, MixedTypes) {
    expect_string_eq("\"num: \" + 123", "num: 123");
    expect_string_eq("\"bool: \" + true", "bool: true");
}
