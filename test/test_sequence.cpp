// test_sequence.cpp — Parser-compiler and parser-compiler-VM tests for the
// sequence protocol: `elem in seq` expression and `for (var x in seq)` loop.

#include "test_harness.h"
#include <gtest/gtest.h>

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

// ===========================================================================
// Parser-Compiler tests — verify emitted bytecode
// ===========================================================================

class SequenceBytecodeTest : public ::testing::Test {};

// ---------------------------------------------------------------------------
// `elem in seq` expression
// ---------------------------------------------------------------------------

// `2 in [1, 2, 3]`
// Constants: slot0='2', slot1='1', slot2='3'  (slot0 deduped for both occurrences of 2)
// Left operand is pushed first, then the right-hand list literal.
TEST_F(SequenceBytecodeTest, InExpr_ListMembership) {
    std::string bytecode = compile_to_bytecode("2 in [1, 2, 3]");
    std::string expected = "0: CONSTANT 0 ('2')\n"
                           "2: CONSTANT 1 ('1')\n"
                           "4: CONSTANT 0 ('2')\n" // dedup: reuses slot 0
                           "6: CONSTANT 2 ('3')\n"
                           "8: BUILD_LIST 3\n"
                           "10: IN\n"
                           "11: POP\n"
                           "12: NIL\n"
                           "13: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

// `"ell" in "hello"`
// Both string literals become constants; IN is emitted after both operands.
TEST_F(SequenceBytecodeTest, InExpr_StringSubstring) {
    std::string bytecode = compile_to_bytecode("\"ell\" in \"hello\"");
    std::string expected = "0: CONSTANT 0 ('ell')\n"
                           "2: CONSTANT 1 ('hello')\n"
                           "4: IN\n"
                           "5: POP\n"
                           "6: NIL\n"
                           "7: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

// ---------------------------------------------------------------------------
// `for (var x in seq)` loop
// ---------------------------------------------------------------------------
//
// for (var x in [1]) {} compiles to (at script level):
//
//   Locals: slot0="" (implicit), slot1=(seq), slot2=(idx), slot3=x
//   Constants: slot0='1' (list elem and increment 1.0, deduped),
//              slot1='0' (initial idx), slot2='len' (global name)
//
//   0:  CONSTANT 0 ('1')        ← list literal element 1
//   2:  BUILD_LIST 1            ← push [1] as (seq)
//   4:  CONSTANT 1 ('0')        ← push 0 as (idx)
//   6:  NIL                     ← push nil as item x
//   7:  GET_LOCAL 2             ← condition: push (idx)
//   9:  GET_GLOBAL 2 ('len')    ← push len function
//   11: GET_LOCAL 1             ← push (seq)
//   13: CALL 1                  ← len(seq)
//   15: LESS                    ← idx < len(seq)
//   16: JUMP_IF_FALSE 16 -> 45  ← exit if false
//   19: POP
//   20: JUMP 20 -> 34           ← jump over increment to body
//   23: GET_LOCAL 2             ← increment: push (idx)
//   25: CONSTANT 0 ('1')        ← push 1 (deduped with slot0)
//   27: ADD                     ← idx + 1
//   28: SET_LOCAL 2             ← store back into (idx)
//   30: POP
//   31: LOOP 31 -> 7            ← back to condition (loopStart)
//   34: GET_LOCAL 1             ← body: push (seq)
//   36: GET_LOCAL 2             ← push (idx)
//   38: GET_INDEX               ← seq[idx]
//   39: SET_LOCAL 3             ← x = seq[idx]
//   41: POP
//   42: LOOP 42 -> 23           ← back to increment (incrStart)
//   45: POP                     ← discard condition result on exit
//   46: POP                     ← endScope: pop x
//   47: POP                     ← endScope: pop (idx)
//   48: POP                     ← endScope: pop (seq)
//   49: NIL
//   50: RETURN
TEST_F(SequenceBytecodeTest, ForIn_EmptyBodyBytecode) {
    std::string bytecode = compile_program_to_bytecode("for (var x in [1]) {}");
    std::string expected =
        "0: CONSTANT 0 ('1')\n"
        "2: BUILD_LIST 1\n"
        "4: CONSTANT 1 ('0')\n"
        "6: NIL\n"
        "7: GET_LOCAL 2\n"
        "9: GET_GLOBAL 2 ('len')\n"
        "11: GET_LOCAL 1\n"
        "13: CALL 1\n"
        "15: LESS\n"
        "16: JUMP_IF_FALSE 16 -> 45\n"
        "19: POP\n"
        "20: JUMP 20 -> 34\n"
        "23: GET_LOCAL 2\n"
        "25: CONSTANT 0 ('1')\n" // dedup: 1.0 reuses list-element slot
        "27: ADD\n"
        "28: SET_LOCAL 2\n"
        "30: POP\n"
        "31: LOOP 31 -> 7\n"
        "34: GET_LOCAL 1\n"
        "36: GET_LOCAL 2\n"
        "38: GET_INDEX\n"
        "39: SET_LOCAL 3\n"
        "41: POP\n"
        "42: LOOP 42 -> 23\n"
        "45: POP\n"
        "46: POP\n" // endScope: x
        "47: POP\n" // endScope: (idx)
        "48: POP\n" // endScope: (seq)
        "49: NIL\n"
        "50: RETURN\n";
    EXPECT_EQ(trim(bytecode), trim(expected));
}

// ===========================================================================
// Parser-Compiler-VM tests — verify runtime semantics
// ===========================================================================

// ---------------------------------------------------------------------------
// `elem in list` — List membership
// ---------------------------------------------------------------------------

TEST(SequenceVMTest, InList_Found) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var r = 2 in [1, 2, 3];"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "true");
}

TEST(SequenceVMTest, InList_NotFound) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var r = 9 in [1, 2, 3];"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "false");
}

TEST(SequenceVMTest, InList_EmptyList) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var r = 1 in [];"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "false");
}

TEST(SequenceVMTest, InList_StringElement) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var r = "b" in ["a", "b", "c"];)"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "true");
}

TEST(SequenceVMTest, InList_BoolElement) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var r = true in [false, true];"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "true");
}

TEST(SequenceVMTest, InList_NilElement) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var r = nil in [1, nil, 3];"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "true");
}

TEST(SequenceVMTest, InList_UsedInCondition) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = 0;
        if (2 in [1, 2, 3]) { x = 1; }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("x"), "1");
}

TEST(SequenceVMTest, InList_RuntimeError_NonListNonString) {
    VMTestHarness h;
    ASSERT_EQ(h.run("1 in 42;"), InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// `elem in string` — Substring membership
// ---------------------------------------------------------------------------

TEST(SequenceVMTest, InString_SubstringFound) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var r = "ell" in "hello";)"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "true");
}

TEST(SequenceVMTest, InString_SubstringNotFound) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var r = "xyz" in "hello";)"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "false");
}

TEST(SequenceVMTest, InString_SingleCharFound) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var r = "h" in "hello";)"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "true");
}

TEST(SequenceVMTest, InString_EmptyNeedle) {
    // Empty string is a substring of every string.
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var r = "" in "hello";)"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "true");
}

TEST(SequenceVMTest, InString_EmptyHaystack) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var r = "x" in "";)"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "false");
}

TEST(SequenceVMTest, InString_RuntimeError_NonStringElem) {
    // Left operand of `in` on a string must itself be a string.
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(1 in "hello";)"), InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// String indexing
// ---------------------------------------------------------------------------

TEST(SequenceVMTest, StringIndex_FirstChar) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var r = "hello"[0];)"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "h");
}

TEST(SequenceVMTest, StringIndex_LastChar) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var r = "hello"[4];)"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "o");
}

TEST(SequenceVMTest, StringIndex_MiddleChar) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var r = "hello"[2];)"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "l");
}

TEST(SequenceVMTest, StringIndex_OutOfBounds) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"("hello"[5];)"), InterpretResult::RUNTIME_ERROR);
}

TEST(SequenceVMTest, StringIndex_SetIsError) {
    // Strings are immutable — assignment via index must be a runtime error.
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var s = "hello"; s[0] = "x";)"),
              InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// `len` on strings
// ---------------------------------------------------------------------------

TEST(SequenceVMTest, Len_EmptyString) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var r = len("");)"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "0");
}

TEST(SequenceVMTest, Len_NonEmptyString) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var r = len("hello");)"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "5");
}

// ---------------------------------------------------------------------------
// `for (var x in list)` — iteration
// ---------------------------------------------------------------------------

TEST(SequenceVMTest, ForIn_List_SumElements) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var sum = 0;
        for (var x in [1, 2, 3]) { sum = sum + x; }
        var r = sum;
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "6");
}

TEST(SequenceVMTest, ForIn_List_EmptyList) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var r = 0;
        for (var x in []) { r = r + 1; }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "0");
}

TEST(SequenceVMTest, ForIn_List_SingleElement) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var r = 0;
        for (var x in [42]) { r = x; }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "42");
}

TEST(SequenceVMTest, ForIn_List_ItemVariableScoped) {
    // Loop variable must not leak into the enclosing scope.
    VMTestHarness h;
    // After the loop, `x` should not be defined as a global.
    ASSERT_EQ(h.run(R"(
        for (var x in [1]) {}
    )"),
              InterpretResult::OK);
    EXPECT_FALSE(h.getGlobal("x").has_value());
}

TEST(SequenceVMTest, ForIn_List_BreakExitsLoop) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var n = 0;
        for (var x in [1, 2, 3, 4, 5]) {
            if (x == 3) break;
            n = n + x;
        }
        var r = n;
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "3"); // 1 + 2
}

TEST(SequenceVMTest, ForIn_List_ContinueSkipsRest) {
    // continue should jump to the increment (idx++), not re-enter the condition.
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var n = 0;
        for (var x in [1, 2, 3, 4]) {
            if (x == 2) continue;
            n = n + x;
        }
        var r = n;
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "8"); // 1 + 3 + 4
}

TEST(SequenceVMTest, ForIn_List_Nested) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var acc = 0;
        for (var a in [1, 2]) {
            for (var b in [10, 20]) {
                acc = acc + a + b;
            }
        }
        var r = acc;
    )"),
              InterpretResult::OK);
    // a=1: 11 + 21 = 32; a=2: 12 + 22 = 34; total = 66
    EXPECT_EQ(h.getGlobalStr("r"), "66");
}

// ---------------------------------------------------------------------------
// `for (var c in string)` — character iteration
// ---------------------------------------------------------------------------

TEST(SequenceVMTest, ForIn_String_ConcatChars) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var r = "";
        for (var c in "abc") { r = r + c; }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "abc");
}

TEST(SequenceVMTest, ForIn_String_EmptyString) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var r = 0;
        for (var c in "") { r = r + 1; }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "0");
}

TEST(SequenceVMTest, ForIn_String_SingleChar) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var r = "";
        for (var c in "x") { r = r + c; }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "x");
}

TEST(SequenceVMTest, ForIn_String_BreakExitsLoop) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var r = "";
        for (var c in "hello") {
            if (c == "l") break;
            r = r + c;
        }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "he");
}

// ---------------------------------------------------------------------------
// Regression: C-style for loop still works after the for-in change
// ---------------------------------------------------------------------------

TEST(SequenceVMTest, CStyleFor_Regression) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var s = 0;
        for (var i = 0; i < 3; i = i + 1) { s = s + i; }
        var r = s;
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "3"); // 0 + 1 + 2
}

TEST(SequenceVMTest, CStyleFor_WithBreak_Regression) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var s = 0;
        for (var i = 0; i < 10; i = i + 1) {
            if (i == 3) break;
            s = s + i;
        }
        var r = s;
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "3"); // 0 + 1 + 2
}
