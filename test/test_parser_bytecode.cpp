#include "test_harness.h"
#include <gtest/gtest.h>

// Helper to trim whitespace for easier bytecode string comparison
#include <algorithm>
#include <cctype>
#include <string>

static std::string trim(const std::string& s) {
    auto start = s.begin();
    while (start != s.end() && std::isspace(*start))
        ++start;
    auto end = s.end();
    do {
        --end;
    } while (end != start && std::isspace(*end));
    return std::string(start, end + 1);
}

class ParserBytecodeTest : public ::testing::Test {};

TEST_F(ParserBytecodeTest, Arithmetic_Addition) {
    std::string expr = "1 + 2";
    std::string bytecode = compile_to_bytecode(expr);
    std::string expected = "0: CONSTANT 0 ('1')\n"
                           "2: CONSTANT 1 ('2')\n"
                           "4: ADD\n"
                           "5: POP\n"
                           "6: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

TEST_F(ParserBytecodeTest, Arithmetic_MultiplySubtract) {
    std::string expr = "3 * 4 - 5";
    std::string bytecode = compile_to_bytecode(expr);
    std::string expected = "0: CONSTANT 0 ('3')\n"
                           "2: CONSTANT 1 ('4')\n"
                           "4: MULTIPLY\n"
                           "5: CONSTANT 2 ('5')\n"
                           "7: SUBTRACT\n"
                           "8: POP\n"
                           "9: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

TEST_F(ParserBytecodeTest, Comparison_Equal) {
    std::string expr = "1 == 1";
    std::string bytecode = compile_to_bytecode(expr);
    // After constant-pool deduplication both '1' literals share slot 0.
    std::string expected = "0: CONSTANT 0 ('1')\n"
                           "2: CONSTANT 0 ('1')\n"
                           "4: EQUAL\n"
                           "5: POP\n"
                           "6: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

TEST_F(ParserBytecodeTest, Comparison_LessGreater) {
    std::string expr = "2 < 3 > 1";
    std::string bytecode = compile_to_bytecode(expr);
    std::string expected = "0: CONSTANT 0 ('2')\n"
                           "2: CONSTANT 1 ('3')\n"
                           "4: LESS\n"
                           "5: CONSTANT 2 ('1')\n"
                           "7: GREATER\n"
                           "8: POP\n"
                           "9: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

TEST_F(ParserBytecodeTest, Boolean_TrueFalseNot) {
    std::string expr = "!false == true";
    std::string bytecode = compile_to_bytecode(expr);
    std::string expected = "0: FALSE\n"
                           "1: NOT\n"
                           "2: TRUE\n"
                           "3: EQUAL\n"
                           "4: POP\n"
                           "5: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

TEST_F(ParserBytecodeTest, String_Concatenation) {
    std::string expr = "\"foo\" + \"bar\"";
    std::string bytecode = compile_to_bytecode(expr);
    std::string expected = "0: CONSTANT 0 ('foo')\n"
                           "2: CONSTANT 1 ('bar')\n"
                           "4: ADD\n"
                           "5: POP\n"
                           "6: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

TEST_F(ParserBytecodeTest, GroupingAndNegate) {
    std::string expr = "-(1 + 2)";
    std::string bytecode = compile_to_bytecode(expr);
    std::string expected = "0: CONSTANT 0 ('1')\n"
                           "2: CONSTANT 1 ('2')\n"
                           "4: ADD\n"
                           "5: NEGATE\n"
                           "6: POP\n"
                           "7: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

TEST_F(ParserBytecodeTest, NilLiteral) {
    std::string expr = "nil";
    std::string bytecode = compile_to_bytecode(expr);
    std::string expected = "0: NIL\n"
                           "1: POP\n"
                           "2: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

// ===========================================================================
// Constant pool deduplication
// ===========================================================================
// Regression tests: the same value must occupy only one constant slot,
// regardless of how many times it appears in source.

TEST_F(ParserBytecodeTest, Dedup_SameVariableReusesConstantSlot) {
    // The initializer expression is now compiled before identifierConstant("x")
    // is called (for globals), so '1' lands in slot 0 and 'x' in slot 1.
    // Both subsequent references reuse those same slots — no new constant
    // entries.
    std::string src = "var x = 1;\n"
                      "x = x + 1;";
    std::string bytecode = compile_program_to_bytecode(src);
    std::string expected = "0: CONSTANT 0 ('1')\n" // initializer: 1 → slot 0
                           "2: DEFINE_GLOBAL 1 ('x')\n" // x → slot 1
                           "4: GET_GLOBAL 1 ('x')\n"    // x reuses slot 1
                           "6: CONSTANT 0 ('1')\n"      // 1 reuses slot 0
                           "8: ADD\n"
                           "9: SET_GLOBAL 1 ('x')\n" // x reuses slot 1
                           "11: POP\n"
                           "12: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}
