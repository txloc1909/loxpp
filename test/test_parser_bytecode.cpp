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
    // CONSTANT now takes 3 bytes (opcode + 2-byte index).
    std::string expected = "0: CONSTANT 0 ('1')\n"
                           "3: CONSTANT 1 ('2')\n"
                           "6: ADD\n"
                           "7: POP\n"
                           "8: NIL\n"
                           "9: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

TEST_F(ParserBytecodeTest, Arithmetic_MultiplySubtract) {
    std::string expr = "3 * 4 - 5";
    std::string bytecode = compile_to_bytecode(expr);
    std::string expected = "0: CONSTANT 0 ('3')\n"
                           "3: CONSTANT 1 ('4')\n"
                           "6: MULTIPLY\n"
                           "7: CONSTANT 2 ('5')\n"
                           "10: SUBTRACT\n"
                           "11: POP\n"
                           "12: NIL\n"
                           "13: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

TEST_F(ParserBytecodeTest, Comparison_Equal) {
    std::string expr = "1 == 1";
    std::string bytecode = compile_to_bytecode(expr);
    // After constant-pool deduplication both '1' literals share slot 0.
    std::string expected = "0: CONSTANT 0 ('1')\n"
                           "3: CONSTANT 0 ('1')\n"
                           "6: EQUAL\n"
                           "7: POP\n"
                           "8: NIL\n"
                           "9: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

TEST_F(ParserBytecodeTest, Comparison_LessGreater) {
    std::string expr = "2 < 3 > 1";
    std::string bytecode = compile_to_bytecode(expr);
    std::string expected = "0: CONSTANT 0 ('2')\n"
                           "3: CONSTANT 1 ('3')\n"
                           "6: LESS\n"
                           "7: CONSTANT 2 ('1')\n"
                           "10: GREATER\n"
                           "11: POP\n"
                           "12: NIL\n"
                           "13: RETURN\n";
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
                           "5: NIL\n"
                           "6: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

TEST_F(ParserBytecodeTest, String_Concatenation) {
    std::string expr = "\"foo\" + \"bar\"";
    std::string bytecode = compile_to_bytecode(expr);
    std::string expected = "0: CONSTANT 0 ('foo')\n"
                           "3: CONSTANT 1 ('bar')\n"
                           "6: ADD\n"
                           "7: POP\n"
                           "8: NIL\n"
                           "9: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

TEST_F(ParserBytecodeTest, GroupingAndNegate) {
    std::string expr = "-(1 + 2)";
    std::string bytecode = compile_to_bytecode(expr);
    std::string expected = "0: CONSTANT 0 ('1')\n"
                           "3: CONSTANT 1 ('2')\n"
                           "6: ADD\n"
                           "7: NEGATE\n"
                           "8: POP\n"
                           "9: NIL\n"
                           "10: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

TEST_F(ParserBytecodeTest, NilLiteral) {
    std::string expr = "nil";
    std::string bytecode = compile_to_bytecode(expr);
    std::string expected = "0: NIL\n"
                           "1: POP\n"
                           "2: NIL\n"
                           "3: RETURN\n";
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
    std::string expected = "0: CONSTANT 0 ('1')\n"  // initializer: 1 → slot 0
                           "3: DEFINE_GLOBAL 1 ('x')\n" // x → slot 1
                           "6: GET_GLOBAL 1 ('x')\n"    // x reuses slot 1
                           "9: CONSTANT 0 ('1')\n"      // 1 reuses slot 0
                           "12: ADD\n"
                           "13: SET_GLOBAL 1 ('x')\n" // x reuses slot 1
                           "16: POP\n"
                           "17: NIL\n"
                           "18: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

// ===========================================================================
// Function declaration bytecode tests
// ===========================================================================

class FunctionBytecodeTest : public ::testing::Test {};

// fun foo() {} — empty body, no params.
// Outer (script) chunk: CLOSURE(<fn foo>) + DEFINE_GLOBAL('foo') + NIL+RETURN
// Inner (foo) chunk: NIL + RETURN (from emitReturn())
TEST_F(FunctionBytecodeTest, EmptyFunction_OuterChunk) {
    // Constant 0 = 'foo' (global name), constant 1 = <fn foo>
    std::string bytecode = compile_program_to_bytecode("fun foo() {}");
    std::string expected = "0: CLOSURE 1 ('<fn foo>')\n"
                           "3: DEFINE_GLOBAL 0 ('foo')\n"
                           "6: NIL\n"
                           "7: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

TEST_F(FunctionBytecodeTest, EmptyFunction_InnerChunk) {
    std::string bytecode = compile_fn_body_to_bytecode("fun foo() {}");
    std::string expected = "0: NIL\n"
                           "1: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

// fun add(a, b) { return a + b; }
// Inner chunk: GET_LOCAL 1 (a), GET_LOCAL 2 (b), ADD, RETURN, NIL, RETURN
// (The NIL+RETURN at the end is dead code emitted by endCompiler().)
TEST_F(FunctionBytecodeTest, FunctionWithParams_InnerChunk) {
    std::string src = "fun add(a, b) { return a + b; }";
    std::string bytecode = compile_fn_body_to_bytecode(src);
    std::string expected = "0: GET_LOCAL 1\n" // a — slot 1 (slot 0 = fn)
                           "2: GET_LOCAL 2\n" // b — slot 2
                           "4: ADD\n"
                           "5: RETURN\n"
                           "6: NIL\n" // dead code from endCompiler()
                           "7: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

// fun id(x) { return x; } — single param, return it
TEST_F(FunctionBytecodeTest, FunctionSingleParam_InnerChunk) {
    std::string src = "fun id(x) { return x; }";
    std::string bytecode = compile_fn_body_to_bytecode(src);
    std::string expected = "0: GET_LOCAL 1\n"
                           "2: RETURN\n"
                           "3: NIL\n"
                           "4: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

// fun noReturn() { var x = 1; } — no explicit return; body local is NOT
// explicitly POPped: RETURN resets stackTop to frame->slots, cleaning the
// frame in one shot. No endScope() is called for the outermost function scope.
TEST_F(FunctionBytecodeTest, FunctionWithLocalVar_InnerChunk) {
    std::string src = "fun noReturn() { var x = 1; }";
    std::string bytecode = compile_fn_body_to_bytecode(src);
    std::string expected = "0: CONSTANT 0 ('1')\n"
                           "3: NIL\n"
                           "4: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

TEST_F(ParserBytecodeTest, Arithmetic_Modulo) {
    std::string expr = "10 % 3";
    std::string bytecode = compile_to_bytecode(expr);
    std::string expected = "0: CONSTANT 0 ('10')\n"
                           "3: CONSTANT 1 ('3')\n"
                           "6: MODULO\n"
                           "7: POP\n"
                           "8: NIL\n"
                           "9: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

// ===========================================================================
// Sequence protocol bytecode tests
// ===========================================================================

class SequenceBytecodeTest : public ::testing::Test {};

// `2 in [1, 2, 3]`
// Chunk::addConstant does an O(N) dedup scan (see src/chunk.cpp), so the
// second occurrence of 2.0 (inside the list literal) reuses slot 0 instead
// of allocating a new slot.
// Slot layout: 0='2', 1='1', 2='3'
TEST_F(SequenceBytecodeTest, InExpr_ListMembership) {
    std::string bytecode = compile_to_bytecode("2 in [1, 2, 3]");
    std::string expected = "0: CONSTANT 0 ('2')\n"
                           "3: CONSTANT 1 ('1')\n"
                           "6: CONSTANT 0 ('2')\n" // dedup: reuses slot 0
                           "9: CONSTANT 2 ('3')\n"
                           "12: BUILD_LIST 3\n"
                           "14: IN\n"
                           "15: POP\n"
                           "16: NIL\n"
                           "17: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

// `"ell" in "hello"`: two constant loads then IN.
TEST_F(SequenceBytecodeTest, InExpr_StringSubstring) {
    std::string bytecode = compile_to_bytecode("\"ell\" in \"hello\"");
    std::string expected = "0: CONSTANT 0 ('ell')\n"
                           "3: CONSTANT 1 ('hello')\n"
                           "6: IN\n"
                           "7: POP\n"
                           "8: NIL\n"
                           "9: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

// `for (var x in [1]) {}` compiles to (at script level):
//
//   Locals: slot0="" (implicit), slot1=(iter), slot2=x
//   Constants: slot0='1' (list element)
//
// GET_ITER replaces the list on the stack with an ObjIterator in-place.
// Each loop iteration: GET_LOCAL 1 (iter copy) → ITER_HAS_NEXT → bool check,
// then GET_LOCAL 1 (iter copy) → ITER_NEXT → element → SET_LOCAL 2.
// continue re-enters at loopStart (offset 7), which re-checks ITER_HAS_NEXT.
TEST_F(SequenceBytecodeTest, ForIn_EmptyBodyBytecode) {
    std::string bytecode = compile_program_to_bytecode("for (var x in [1]) {}");
    std::string expected =
        "0: CONSTANT 0 ('1')\n" // list element (3 bytes)
        "3: BUILD_LIST 1\n"     // push [1]
        "5: GET_ITER\n"    // replace [1] with ObjIterator → (iter) at slot 1
        "6: NIL\n"         // push nil → x at slot 2
        "7: GET_LOCAL 1\n" // loopStart: push (iter) copy
        "9: ITER_HAS_NEXT\n" // pop copy, push bool
        "10: JUMP_IF_FALSE 10 -> 23\n"
        "13: POP\n"         // discard true
        "14: GET_LOCAL 1\n" // push (iter) copy
        "16: ITER_NEXT\n"   // pop copy, push next element + advance cursor
        "17: SET_LOCAL 2\n" // x = element
        "19: POP\n"
        "20: LOOP 20 -> 7\n" // back to loopStart
        "23: POP\n"          // discard false
        "24: POP\n"          // endScope: x
        "25: POP\n"          // endScope: (iter)
        "26: NIL\n"
        "27: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

// ===========================================================================
// Match statement bytecode tests
// ===========================================================================

class MatchByteCodeTest : public ::testing::Test {};

// Single literal arm with wildcard: verify EQUAL+JIF+POP chain and
// MATCH_ERROR suppressed by wildcard.
//
//   match 1 { case 1 => x = 1; case _ => x = 0; }
//
// Expected code (subject at local slot 1, program-level local 'x' at slot 0):
//   CONSTANT(1)           push subject
//   GET_LOCAL 1           hidden (match) local — subject
//   CONSTANT(1)           test value
//   EQUAL
//   JUMP_IF_FALSE → miss  if false, skip arm
//   POP                   pop true
//   [body: x=1]
//   POP                   pop binding locals (none here)
//   JUMP → end
//   [miss:] POP           pop false
//   GET_LOCAL 1           wildcard arm — no test emitted
//   [body: x=0]
//   POP                   no binding locals
//   JUMP → end
//   [end:]                no MATCH_ERROR (wildcard present)
TEST_F(MatchByteCodeTest, SingleLiteralPlusWildcard) {
    std::string src = "var x = 0;\n"
                      "match 1 {\n"
                      "    case 1 => x = 1\n"
                      "    case _ => x = 0\n"
                      "};\n";
    // Just verify it compiles and contains MATCH_ERROR-free sequences.
    // We don't fix bytecode offsets here — that's brittle; we verify the
    // presence/absence of MATCH_ERROR via the disassembly string.
    std::string bytecode = compile_program_to_bytecode(src);
    EXPECT_EQ(bytecode.find("MATCH_ERROR"), std::string::npos)
        << "wildcard arm should suppress MATCH_ERROR";
    EXPECT_NE(bytecode.find("EQUAL"), std::string::npos)
        << "literal arm must emit EQUAL";
    EXPECT_NE(bytecode.find("JUMP_IF_FALSE"), std::string::npos)
        << "literal arm must emit JUMP_IF_FALSE";
}

// No wildcard arm: MATCH_ERROR must be emitted.
TEST_F(MatchByteCodeTest, NoWildcardEmitsMatchError) {
    std::string src = "var x = 0;\n"
                      "match 1 {\n"
                      "    case 1 => x = 1\n"
                      "    case 2 => x = 2\n"
                      "};\n";
    std::string bytecode = compile_program_to_bytecode(src);
    EXPECT_NE(bytecode.find("MATCH_ERROR"), std::string::npos)
        << "missing wildcard must emit MATCH_ERROR";
}

// Binding arm: GET_LOCAL for subject is emitted (to push binding value).
// MATCH_ERROR is suppressed because the binding always matches.
TEST_F(MatchByteCodeTest, BindingArmEmitsGetLocal) {
    std::string src = "match 42 {\n"
                      "    case n => n\n"
                      "};\n";
    std::string bytecode = compile_program_to_bytecode(src);
    EXPECT_EQ(bytecode.find("MATCH_ERROR"), std::string::npos)
        << "binding arm (always matches) must suppress MATCH_ERROR";
    EXPECT_NE(bytecode.find("GET_LOCAL"), std::string::npos)
        << "binding arm must emit GET_LOCAL to push subject value";
}

// Guard arm: JUMP_IF_FALSE appears twice — once for the guard, in addition
// to any literal test.
TEST_F(MatchByteCodeTest, GuardEmitsExtraJumpIfFalse) {
    std::string src = "match 5 {\n"
                      "    case n if n > 3 => n\n"
                      "    case _ => 0\n"
                      "};\n";
    std::string bytecode = compile_program_to_bytecode(src);
    // Count JUMP_IF_FALSE occurrences: guard adds one.
    size_t count = 0;
    size_t pos = 0;
    while ((pos = bytecode.find("JUMP_IF_FALSE", pos)) != std::string::npos) {
        ++count;
        pos += 1;
    }
    EXPECT_GE(count, 1u) << "guard must emit at least one JUMP_IF_FALSE";
}
