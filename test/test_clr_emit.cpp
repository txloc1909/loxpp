// test_clr_emit.cpp — CLR emitter: straight-line code, control flow,
// functions and calls, and closures and upvalues (the bug gate).
//
// Checkpoint (see the node's own specification): `tools/loxpp_clr.sh` on
// every probe in `tools/check_clr_probes.sh`'s accumulated list must print
// stdout identical to build/loxpp, `V1_fresh_cell` must print `0 1 2` (never
// `2 2 2`), and the error probes must FAIL identically on both sides. That
// full assemble-and-run comparison needs ilasm/dotnet
// (tools/check_clr_probes.sh, run inside the dev-managed container); this
// file covers what a plain C++ unit test can check without them: the
// bytearray/bit-pattern literal helpers, and the generated ilasm's
// structural shape (locals, maxstack, fusion, abort-on-unsupported,
// multi-class programs via emitProgram, the idempotent captured-cell seed,
// and the upvalue array wiring).

#include "backend/abstract_stack.h"
#include "backend/chunk_decoder.h"
#include "backend/clr_emitter.h"
#include "backend/zero_depth_local.h"
#include "compiler.h"
#include "memory_manager.h"
#include "object.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

DecodedFunction decodeScript(const std::string& source, MemoryManager& mm) {
    ObjFunction* script = compile(source, &mm);
    if (script == nullptr) {
        throw std::runtime_error("compilation failed");
    }
    return decodeFunctionTree(script);
}

int countOccurrences(const std::string& haystack, const std::string& needle) {
    int count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        count++;
        pos += needle.size();
    }
    return count;
}

// Every branch mnemonic this pass emits. A new one needs its own entry
// here too, or that branch gets no label-integrity coverage from this
// helper (test_jvm_emit.cpp's own kJumpMnemonics note).
const std::vector<std::string> kBranchMnemonics = {"br ", "brtrue ",
                                                   "brfalse "};

// For every branch instruction in `il`, confirms its own target text names
// a label this pass actually wrote (a "<name>:" line) — not merely that a
// `br`/`brtrue` text token is present. `br`/`brtrue` to a label ilasm never
// wrote assembles as a hard ilasm error ("Unable to find forward reference
// label"), only caught by tools/check_clr_probes.sh (ilasm, container-only)
// — a plain C++ unit test that stopped at "does the text contain 'br '"
// would miss exactly this class of bug (the fusablePop CFG-label guard's
// own hazard, probe 22).
void expectEveryBranchTargetIsLabeled(const std::string& il) {
    const std::string text = "\n" + il;
    for (const std::string& mnemonic : kBranchMnemonics) {
        const std::string anchored = "\n    " + mnemonic;
        std::size_t pos = 0;
        while ((pos = text.find(anchored, pos)) != std::string::npos) {
            std::size_t nameStart = pos + anchored.size();
            std::size_t nameEnd = text.find('\n', nameStart);
            ASSERT_NE(nameEnd, std::string::npos)
                << mnemonic << " operand runs off the end of:\n"
                << il;
            std::string target = text.substr(nameStart, nameEnd - nameStart);
            EXPECT_NE(text.find("\n" + target + ":\n"), std::string::npos)
                << "branch to undefined label \"" << target << "\" in:\n"
                << il;
            pos = nameEnd;
        }
    }
}

// ---------------------------------------------------------------------------
// ilasmStringLiteral
// ---------------------------------------------------------------------------

TEST(IlasmStringLiteral, EncodesEachByteAsATwoByteLittleEndianCodeUnit) {
    EXPECT_EQ(clr::ilasmStringLiteral("AB"), "bytearray (41 00 42 00)");
}

TEST(IlasmStringLiteral, EncodesQuoteBackslashAndNewlineLikeAnyOtherByte) {
    // These three bytes are exactly the ones ilasm's own quoted-string
    // lexer treats specially; the bytearray form does not need to, and
    // does not.
    std::string lit = clr::ilasmStringLiteral("\"\\\n");
    EXPECT_EQ(lit, "bytearray (22 00 5c 00 0a 00)");
}

TEST(IlasmStringLiteral, EncodesAHighByteAsItsOwnCodePointNotUtf8) {
    // Measured against ilasm 8.0.0: its quoted-string octal escape for this
    // same byte (`\377`) comes back as U+FFFD once ilasm's own UTF-8 source
    // reader decodes it — the bytearray form has no such failure mode.
    std::string lit = clr::ilasmStringLiteral(std::string(1, '\xff'));
    EXPECT_EQ(lit, "bytearray (ff 00)");
}

TEST(IlasmStringLiteral, EncodesAnEmbeddedNulByte) {
    // Measured against ilasm 8.0.0: a literal NUL byte in a quoted string
    // ends its own string token early (`"a\0b"` assembles as if written
    // `"a"`, with a syntax error right after), and its `\0` escape silently
    // truncates the string value at that point instead of raising one — the
    // bytearray form has neither failure mode, because ilasm's own
    // string-token lexer never runs on it.
    std::string lit =
        clr::ilasmStringLiteral(std::string("a") + std::string(1, '\0') + "b");
    EXPECT_EQ(lit, "bytearray (61 00 00 00 62 00)");
}

// ---------------------------------------------------------------------------
// ilasmDoubleLiteral
// ---------------------------------------------------------------------------

TEST(IlasmDoubleLiteral, RoundTripsExactly) {
    double values[] = {2.0,
                       -6.0,
                       0.5,
                       0.1,
                       1.0 / 3.0,
                       0.0,
                       -0.0,
                       1e300,
                       -1e-300,
                       16777217.0,
                       1e17,
                       3.14159265358979,
                       std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity()};
    for (double v : values) {
        std::string literal = clr::ilasmDoubleLiteral(v);
        ASSERT_EQ(literal.front(), '(');
        ASSERT_EQ(literal.back(), ')');
        std::string hex = literal.substr(1, literal.size() - 2);
        // Reassemble the little-endian byte sequence exactly the way
        // ilasm's own `ldc.r8 (...)` operand does.
        std::vector<std::string> bytes;
        std::size_t pos = 0;
        while (pos < hex.size()) {
            bytes.push_back(hex.substr(pos, 2));
            pos += 3; // "xx "
        }
        ASSERT_EQ(bytes.size(), 8u) << literal;
        uint64_t bits = 0;
        for (int i = 7; i >= 0; i--) {
            bits = (bits << 8) |
                   static_cast<uint64_t>(std::stoul(bytes[i], nullptr, 16));
        }
        double roundTripped = std::bit_cast<double>(bits);
        EXPECT_EQ(std::bit_cast<uint64_t>(roundTripped),
                  std::bit_cast<uint64_t>(v))
            << "value " << v << " literal " << literal;
    }
}

// ---------------------------------------------------------------------------
// emitScript
// ---------------------------------------------------------------------------

// This node's own opcode set (clr_emitter.h) has no enum tag dispatch and
// no SLICE/IN/for-in — so JUMP_TABLE is one of the opcodes this pass must
// still refuse rather than silently mis-lower. A hand-built single-
// instruction chunk drives the refusal directly, the same way
// test_jvm_emit.cpp's own `AbortsOnUnsupportedOpcode` does, rather than
// hunting for a real Lox++ program that reaches an opcode this pass has no
// case for.
TEST(EmitScript, AbortsOnUnsupportedOpcode) {
    // CALL/CLOSURE/RETURN/CLASS/INVOKE/MATCH_ERROR are no longer
    // unsupported (this node); JUMP_TABLE is the nearest opcode still
    // outside this pass's scope.
    MemoryManager mm;
    DecodedInstruction table;
    table.offset = 0;
    table.op = Op::JUMP_TABLE;
    table.minTag = 0;
    table.length = 3; // min_tag (1 byte) + count (1 byte), zero arms.

    DecodedFunction fn;
    fn.id = "0";
    fn.function = mm.create<ObjFunction>();
    fn.instructions = {table};

    FunctionStackAnalysis analysis;
    analysis.functionId = "0";
    // JUMP_TABLE pops the tag alone (stackEffect: {1, 0}) — one temporary
    // in, zero out.
    analysis.before = {StackState{1, 0}};
    analysis.after = {StackState{0, 0}};
    analysis.reached = {true};

    try {
        clr::emitScript(fn, analysis, "LoxMain");
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("JUMP_TABLE"), std::string::npos)
            << e.what();
    }
}

TEST(EmitScript, AssignLocalFusesSetLocalWithTempPop) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript("{ var a = 1; a = 2; print a; }", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    EXPECT_NE(j.find(".class public auto ansi LoxMain"), std::string::npos);
    // globals(1) + frame slots [callee, a](2) + scratch(1).
    EXPECT_NE(j.find(".locals init (object, object, object, object)\n"),
              std::string::npos);
    // Every value here is at most one live cell deep at once.
    EXPECT_NE(j.find(".maxstack 1\n"), std::string::npos);
    // The SET_LOCAL;POP(TEMP) idiom folds to one `stloc` — no `dup`
    // anywhere, and the only standalone `pop` left is RETURN's own
    // discard of the trailing NIL (the reclaim POP at the closing brace
    // needs no instruction at all).
    EXPECT_EQ(countOccurrences(j, "dup"), 0);
    EXPECT_EQ(countOccurrences(j, "\n    pop\n"), 1);
    // `a`'s declaring push (the invisible var) and its later reassignment
    // both store to the same local.
    EXPECT_EQ(countOccurrences(j, "stloc 2\n"), 2);
    EXPECT_EQ(countOccurrences(j, "ldloc 2\n"), 1);
}

TEST(EmitScript, NestedArithHasNoLocalsAndNoGlobals) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript("print (1 + 2) * (3 - 4) / 5 - -6;", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    // globals(1) + frame slots [callee](1) + scratch(1).
    EXPECT_NE(j.find(".locals init (object, object, object)\n"),
              std::string::npos);
    EXPECT_NE(j.find("call object [LoxRuntime]Lox.LoxOps::Add"),
              std::string::npos);
    EXPECT_NE(j.find("call object [LoxRuntime]Lox.LoxOps::Subtract"),
              std::string::npos);
    EXPECT_NE(j.find("call object [LoxRuntime]Lox.LoxOps::Multiply"),
              std::string::npos);
    EXPECT_NE(j.find("call object [LoxRuntime]Lox.LoxOps::Divide"),
              std::string::npos);
    EXPECT_NE(j.find("call object [LoxRuntime]Lox.LoxOps::Negate"),
              std::string::npos);
    EXPECT_NE(j.find("call void [LoxRuntime]Lox.LoxOps::Print"),
              std::string::npos);
    // LoxRuntime.Init() still runs (every script initializes globals up
    // front), but this probe never defines, reads, or sets one.
    EXPECT_EQ(j.find("LoxGlobals::Define"), std::string::npos);
    EXPECT_EQ(j.find("LoxGlobals::Get"), std::string::npos);
    EXPECT_EQ(j.find("LoxGlobals::Set"), std::string::npos);
    // `.maxstack` must come from the shared abstract-stack analysis, not a
    // private count — this chunk's own high-water mark is 3 (the moment
    // `(3 - 4)`'s second operand joins `(1 + 2)`'s already-computed result
    // and this expression's own left operand, all three still live at
    // once).
    EXPECT_NE(j.find(".maxstack 3\n"), std::string::npos);
}

TEST(EmitScript, NumberConstantUsesRawBitsRoundTrip) {
    // The float-imprecision trap (16777217, the smallest integer a 32-bit
    // float cannot hold exactly): this must not go through any lexer that
    // could round it, decimal or otherwise.
    MemoryManager mm;
    DecodedFunction fn = decodeScript("print 16777217;", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    std::string expected =
        "ldc.r8 " + clr::ilasmDoubleLiteral(static_cast<double>(16777217)) +
        "\n";
    EXPECT_NE(j.find(expected), std::string::npos) << j;
    EXPECT_NE(j.find("box [System.Runtime]System.Double"), std::string::npos);
}

TEST(EmitScript, GlobalsRoundTripThroughDefineSetGet) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript("var x = 1;\nx = 2;\nprint x;\n", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    EXPECT_NE(j.find("call instance void [LoxRuntime]Lox.LoxGlobals::"
                     "Define(string, object)"),
              std::string::npos);
    EXPECT_NE(j.find("call instance void [LoxRuntime]Lox.LoxGlobals::"
                     "Set(string, object)"),
              std::string::npos);
    EXPECT_NE(j.find("call instance object [LoxRuntime]Lox.LoxGlobals::"
                     "Get(string)"),
              std::string::npos);
    EXPECT_NE(j.find("ldstr " + clr::ilasmStringLiteral("x")),
              std::string::npos);
}

// ---------------------------------------------------------------------------
// Regression: a SET_LOCAL/SET_GLOBAL peek whose source value the shared
// abstract-stack pass already folded into a named local (the eager
// invisible-var materialization, abstract_stack.h) must reload that local,
// not assume a CIL evaluation-stack temporary that was never pushed.
// See clr_emitter.h's own top-of-file note.
// ---------------------------------------------------------------------------

TEST(EmitScript, SetLocalPeekOfNamedLocalLoadsInsteadOfDup) {
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("{ var a = 1; var b = (a = 2); print a; print b; }", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    // globals(1) + frame slots [callee, a, b](3) + scratch(1).
    EXPECT_NE(j.find(".locals init (object, object, object, object, object)\n"),
              std::string::npos);
    // No `dup` anywhere: before[SET_LOCAL(a)].operandDepth() == 0, so the
    // reload-from-slot branch runs, never the dup branch.
    EXPECT_EQ(countOccurrences(j, "dup"), 0);
    // The fix in one line: reload `b`'s slot (3), the value SET_LOCAL(a) is
    // peeking, then store it into `a`'s slot (2).
    EXPECT_NE(j.find("ldloc 3\n    stloc 2\n"), std::string::npos) << j;
    // `a` gets its declaring store and the fixed-up store; `b` gets only
    // its declaring store.
    EXPECT_EQ(countOccurrences(j, "stloc 2\n"), 2);
    EXPECT_EQ(countOccurrences(j, "stloc 3\n"), 1);
}

TEST(EmitScript, SetGlobalPeekOfNamedLocalLoadsInsteadOfDup) {
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("var g = 0; { var b = g = 9; print b; print g; }", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    EXPECT_EQ(countOccurrences(j, "dup"), 0);
    // The fix: `b`'s own declaring store (slot 2) is immediately followed
    // by SET_GLOBAL('g')'s reload of that exact slot, feeding
    // emitGlobalsCall's own shuffle — never a fresh, wrongly-assumed
    // evaluation-stack temporary.
    EXPECT_NE(j.find("stloc 2\n    ldloc 2\n"), std::string::npos) << j;
    // `print b` reads `b`'s slot directly — proof the fix, not a stale or
    // scratch value, is what feeds it.
    EXPECT_NE(j.find("ldloc 2\n    call void [LoxRuntime]Lox.LoxOps::Print"),
              std::string::npos)
        << j;
}

// ---------------------------------------------------------------------------
// Comparisons, MODULO, NOT, and the TRUE/FALSE/NIL literals — the opcode
// family notes/translation-probes/30_bool_compare_and_string_literal.lox
// drives end to end through ilasm/dotnet; these tie the same family to the
// exact instruction text at the C++ level.
// ---------------------------------------------------------------------------

TEST(EmitScript, ComparisonOpsCallLoxOpsAndBoxTheResult) {
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("print 1 == 1;\nprint 1 > 2;\nprint 1 < 2;\n", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    EXPECT_NE(j.find("call bool [LoxRuntime]Lox.LoxOps::Equal(object, "
                     "object)\n    box [System.Runtime]System.Boolean"),
              std::string::npos)
        << j;
    EXPECT_NE(j.find("call bool [LoxRuntime]Lox.LoxOps::Greater(object, "
                     "object)\n    box [System.Runtime]System.Boolean"),
              std::string::npos)
        << j;
    EXPECT_NE(j.find("call bool [LoxRuntime]Lox.LoxOps::Less(object, "
                     "object)\n    box [System.Runtime]System.Boolean"),
              std::string::npos)
        << j;
}

TEST(EmitScript, NotEqualGreaterEqualAndLessEqualComposeWithNot) {
    // compiler.cpp's Compiler::binary: != is EQUAL+NOT, >= is LESS+NOT, <=
    // is GREATER+NOT — no dedicated opcode of its own.
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("print 1 != 2;\nprint 1 >= 2;\nprint 1 <= 2;\n", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    EXPECT_EQ(countOccurrences(j, "LoxOps::Equal"), 1);
    EXPECT_EQ(countOccurrences(j, "LoxOps::Less"), 1);
    EXPECT_EQ(countOccurrences(j, "LoxOps::Greater"), 1);
    EXPECT_EQ(countOccurrences(j, "LoxOps::Not(object)"), 3);
}

TEST(EmitScript, ModuloCallsLoxOpsModulo) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript("print 7 % 3;", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    EXPECT_NE(j.find("call object [LoxRuntime]Lox.LoxOps::Modulo(object, "
                     "object)"),
              std::string::npos)
        << j;
}

TEST(EmitScript, TrueFalseAndNilLoadTheirOwnLiteralAndBoxWhereNeeded) {
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("print true;\nprint false;\nprint nil;", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    EXPECT_NE(j.find("ldc.i4.1\n    box [System.Runtime]System.Boolean\n"),
              std::string::npos)
        << j;
    EXPECT_NE(j.find("ldc.i4.0\n    box [System.Runtime]System.Boolean\n"),
              std::string::npos)
        << j;
    EXPECT_NE(j.find("ldnull\n"), std::string::npos) << j;
}

// ---------------------------------------------------------------------------
// Builder::emit's underflow guard, generalized: a hand-built chunk drives an
// instruction whose read count exceeds what its net stack delta alone would
// reveal (NOT/NEGATE net 0 but read 1; a binary/comparison op nets -1 but
// reads 2), the same shape AbortsOnUnsupportedOpcode uses to drive a refusal
// with no real Lox++ program that reaches the case. A single-instruction
// analysis, matching this node's own opcode set (no CALL, no control flow),
// stands in for the earlier, narrower `dup`-only guard.
// ---------------------------------------------------------------------------

TEST(EmitScript, NotOnAnEmptyEvaluationStackThrows) {
    MemoryManager mm;
    DecodedInstruction notOp;
    notOp.offset = 0;
    notOp.op = Op::NOT;
    notOp.length = 1;

    DecodedFunction fn;
    fn.id = "0";
    fn.function = mm.create<ObjFunction>();
    fn.instructions = {notOp};

    FunctionStackAnalysis analysis;
    analysis.functionId = "0";
    analysis.before = {StackState{0, 0}};
    analysis.after = {StackState{0, 0}};
    analysis.reached = {true};

    try {
        clr::emitScript(fn, analysis, "LoxMain");
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("underflow"), std::string::npos)
            << e.what();
    }
}

TEST(EmitScript, ComparisonOnAnUndersizedEvaluationStackThrows) {
    // A single NIL first, to give the CIL evaluation stack a real depth of
    // 1 — the same depth GREATER's own analysis.before reports — so this
    // exercises the two-cell read the boxed-bool call needs, not the
    // one-cell net delta a weaker check would settle for.
    MemoryManager mm;
    DecodedInstruction nilOp;
    nilOp.offset = 0;
    nilOp.op = Op::NIL;
    nilOp.length = 1;
    DecodedInstruction greaterOp;
    greaterOp.offset = 1;
    greaterOp.op = Op::GREATER;
    greaterOp.length = 1;

    DecodedFunction fn;
    fn.id = "0";
    fn.function = mm.create<ObjFunction>();
    fn.instructions = {nilOp, greaterOp};

    FunctionStackAnalysis analysis;
    analysis.functionId = "0";
    analysis.before = {StackState{0, 0}, StackState{1, 0}};
    analysis.after = {StackState{1, 0}, StackState{0, 0}};
    analysis.reached = {true, true};

    try {
        clr::emitScript(fn, analysis, "LoxMain");
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("underflow"), std::string::npos)
            << e.what();
    }
}

TEST(EmitScript, ReturnWithANonEmptyEvaluationStackThrows) {
    // Two NILs give the simulated stack a real depth of 2 entering RETURN,
    // matching an analysis that (incorrectly, for this test's purpose)
    // reports the same depth there — RETURN's own trailing `pop` only
    // discards one of the two, and `ret` from a void method must not run
    // with the other still present.
    MemoryManager mm;
    DecodedInstruction nil0;
    nil0.offset = 0;
    nil0.op = Op::NIL;
    nil0.length = 1;
    DecodedInstruction nil1;
    nil1.offset = 1;
    nil1.op = Op::NIL;
    nil1.length = 1;
    DecodedInstruction ret;
    ret.offset = 2;
    ret.op = Op::RETURN;
    ret.length = 1;

    DecodedFunction fn;
    fn.id = "0";
    fn.function = mm.create<ObjFunction>();
    fn.instructions = {nil0, nil1, ret};

    FunctionStackAnalysis analysis;
    analysis.functionId = "0";
    analysis.before = {StackState{0, 0}, StackState{1, 0}, StackState{2, 0}};
    analysis.after = {StackState{1, 0}, StackState{2, 0}, StackState{1, 0}};
    analysis.reached = {true, true, true};

    try {
        clr::emitScript(fn, analysis, "LoxMain");
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("empty"), std::string::npos)
            << e.what();
    }
}

TEST(EmitScript, StringConstantUsesTheByteArrayLiteral) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript(R"(print "say \"hi\"\n";)", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    EXPECT_NE(j.find("ldstr " + clr::ilasmStringLiteral("say \"hi\"\n")),
              std::string::npos)
        << j;
}

TEST(EmitScript, ScriptReturnPopsTheTrailingNilBeforeRet) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript("print 1;", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    // ECMA-335 III.3.40: `ret` from a void method needs an empty
    // evaluation stack — the trailing NIL `endCompiler()` always appends
    // must be discarded first.
    EXPECT_NE(j.find("ldnull\n    pop\n    ret\n"), std::string::npos) << j;
}

TEST(EmitScript, MainForwardsItsOwnArgvToSetProgramArgsBeforeInit) {
    // Before this node's own fix, a program could never reach the native
    // `args()` global at all (no CALL); once CALL made it reachable,
    // `Main` still declared no parameter and forwarded nothing, so
    // `args()` always answered empty regardless of the real command line.
    MemoryManager mm;
    DecodedFunction fn = decodeScript("print 1;", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    EXPECT_NE(j.find(".method public static void Main(string[] args) cil "
                     "managed"),
              std::string::npos)
        << j;
    // Order matters: SetProgramArgs must run before Init(), which is the
    // globals prologue the rest of the script body depends on.
    std::size_t argsCall = j.find(
        "ldarg.0\n    call void [LoxRuntime]Lox.LoxRuntime::SetProgramArgs"
        "(string[])\n");
    std::size_t initCall = j.find("call class [LoxRuntime]Lox.LoxGlobals "
                                  "[LoxRuntime]Lox.LoxRuntime::Init()");
    ASSERT_NE(argsCall, std::string::npos) << j;
    ASSERT_NE(initCall, std::string::npos) << j;
    EXPECT_LT(argsCall, initCall) << j;
}

// ---------------------------------------------------------------------------
// Control flow: JUMP, JUMP_IF_FALSE, LOOP (this node). See
// notes/translation-probes/{02,03,04,05,22,23}_*.lox for the checkpoint
// this ties to at the assemble-and-run level (tools/check_clr_probes.sh).
// ---------------------------------------------------------------------------

TEST(EmitScript, IfElseDupsThePeekAndBothPopsAreReal) {
    // 02_if_else: `dup` preserves JUMP_IF_FALSE's peeked condition on the
    // taken edge too, so each side's own, ordinary POP (the fall-through's
    // and the else-target's) discards a real copy.
    MemoryManager mm;
    DecodedFunction fn = decodeScript("if (true) print 1; else print 2;", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    EXPECT_EQ(countOccurrences(j, "dup"), 1) << j;
    EXPECT_NE(j.find("call bool [LoxRuntime]Lox.LoxOps::IsFalsy"),
              std::string::npos)
        << j;
    EXPECT_NE(j.find("brtrue L_"), std::string::npos) << j;
    EXPECT_NE(j.find("br L_"), std::string::npos) << j; // skip the else branch
    // 2 real pops (one per branch's own discard of its condition copy) +
    // 1 for the script's own trailing "ldnull; pop; ret" (every chunk
    // ends this way, unrelated to control flow).
    EXPECT_EQ(countOccurrences(j, "\n    pop\n"), 3) << j;
    expectEveryBranchTargetIsLabeled(j);
}

TEST(EmitScript, AndOrKeepsTheValue) {
    // 03_and_or: the branch target is PRINT (the merge that uses the
    // short-circuit result), not a POP — the `dup`'d copy is what
    // survives to be printed; only the fall-through side's own POP is
    // real.
    MemoryManager mm;
    DecodedFunction fn = decodeScript("print 1 and 2;", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    EXPECT_NE(j.find("dup"), std::string::npos) << j;
    // 1 real pop (the fall-through side's own discard) + 1 for the
    // script's own trailing "ldnull; pop; ret".
    EXPECT_EQ(countOccurrences(j, "\n    pop\n"), 2) << j;
    expectEveryBranchTargetIsLabeled(j);
}

TEST(EmitScript, AndOrAssignmentStatementKeepsTheMergeLabelReal) {
    // Probe 22: the short-circuit merge's own POP can also be a CFG block
    // leader when the right side is an assignment — every edge into it
    // needs a real ilasm label there. `fusablePop` must not fuse that POP
    // away, or the label disappears with it and ilasm fails to assemble
    // ("Unable to find forward reference label").
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("var x = true; var y = 0; x and (y = 1); print y;", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    std::size_t brtruePos = j.find("brtrue L_");
    ASSERT_NE(brtruePos, std::string::npos) << j;
    std::size_t nameStart = brtruePos + 7; // skip "brtrue "
    std::string target =
        j.substr(nameStart, j.find('\n', nameStart) - nameStart);
    // The label this `brtrue` targets must exist as a real "name:" line,
    // and the merge must still have its own, real `pop` — proof the fuse
    // did not eat either one.
    EXPECT_NE(j.find(target + ":\n"), std::string::npos) << j;
    EXPECT_NE(j.find("\n    pop\n"), std::string::npos) << j;
    expectEveryBranchTargetIsLabeled(j);
}

TEST(EmitScript, JumpIfFalseOnAMaterializedConditionLoadsInsteadOfDup) {
    // Probe 23: when a local's initializer is a short-circuit expression,
    // the shared abstract-stack pass's eager invisible-var materialization
    // (P2/P3) moves the condition off the CIL evaluation stack before
    // JUMP_IF_FALSE runs — before[i].operandDepth() == 0. `dup` on that
    // empty stack would trip the depth != operandDepth() safety net in
    // emitBody (proven by temporarily disabling the fix); the actual fix
    // reloads a fresh copy through `loadNamedLocalAtZeroDepth` instead.
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("{ var c = true; var b = c and 2; print b; }", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    EXPECT_EQ(countOccurrences(j, "dup"), 0) << j;
    EXPECT_NE(j.find("call bool [LoxRuntime]Lox.LoxOps::IsFalsy"),
              std::string::npos)
        << j;
    expectEveryBranchTargetIsLabeled(j);
}

TEST(EmitScript, WhileLoopEmitsBackEdgeAndLabel) {
    // 04_while: LOOP lowers to `br`, at a label the shared CFG pass placed
    // at the condition. The back edge's own target must be a real, defined
    // label in this same method — not merely present as `br` text.
    MemoryManager mm;
    DecodedFunction fn = decodeScript(
        "{ var i = 0; while (i < 3) { print i; i = i + 1; } }", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    std::size_t brPos = j.find("br L_");
    ASSERT_NE(brPos, std::string::npos) << j;
    std::size_t nameStart = brPos + 3; // skip "br "
    std::string target =
        j.substr(nameStart, j.find('\n', nameStart) - nameStart);
    EXPECT_NE(j.find(target + ":"), std::string::npos) << j;
    expectEveryBranchTargetIsLabeled(j);
}

TEST(EmitScript, ForLoopHasTwoBackEdges) {
    // 05_for: 2 back edges (LOOP) plus 1 forward skip (JUMP) — 3 `br`
    // instructions total (the leaders algorithm's own verified fact for
    // this desugaring, cfg.cpp).
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("for (var i = 0; i < 3; i = i + 1) print i;", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    EXPECT_EQ(countOccurrences(j, "br L_"), 3) << j;
    expectEveryBranchTargetIsLabeled(j);
}

TEST(EmitScript, IfWithoutElseStillEmitsTheUnconditionalSkip) {
    // No else branch: Compiler::ifStatement (compiler.cpp) emits the
    // unconditional "skip the else" JUMP unconditionally too, even with
    // nothing to skip — this pass lowers whatever the compiler emitted,
    // not a structural guess of when a JUMP "should" be there.
    MemoryManager mm;
    DecodedFunction fn = decodeScript("if (true) print 1;\nprint 2;", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    EXPECT_NE(j.find("brtrue L_"), std::string::npos) << j;
    EXPECT_EQ(countOccurrences(j, "br L_"), 1) << j;
    expectEveryBranchTargetIsLabeled(j);
}

// ---------------------------------------------------------------------------
// A SET_LOCAL whose merge-exact operandDepth() is nonzero must resolve its
// slot the same way on every incoming edge (the block-leader depth-resync
// fix, proven to matter by temporarily disabling it: the depth !=
// operandDepth() safety net in emitBody then tripped with "simulated stack
// depth 0 disagrees with analysis depth 1" on 02_if_else).
// ---------------------------------------------------------------------------

TEST(EmitScript, IfElseAssignsSameSlotOnBothBranches) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript(
        "{ var a = 1; if (a == 1) { a = 2; } else { a = 3; } print a; }", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    // `a` is the only local (Lox slot 1) -> CLR slot 2. Both branches must
    // store to it, not to two different slots.
    EXPECT_EQ(countOccurrences(j, "stloc 2\n"), 3) << j; // decl + both branches
    expectEveryBranchTargetIsLabeled(j);
}

TEST(EmitScript, LoopBodyAssignsSameSlotEveryIteration) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript(
        "{ var a = 1; var i = 0; while (i < 3) { a = a + 1; i = i + 1; } }",
        mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    // `a` (CLR slot 2) is reassigned once per loop body pass; the same
    // slot must appear each time this pass walks the (single, static)
    // body.
    EXPECT_NE(j.find("stloc 2\n"), std::string::npos) << j;
    expectEveryBranchTargetIsLabeled(j);
}

// ---------------------------------------------------------------------------
// Functions and calls (this node): CALL, zero-upvalue CLOSURE, RETURN's
// dual role, and emitProgram's multi-class output. See
// notes/translation-probes/{08,24}_*.lox for the checkpoint this ties to at
// the assemble-and-run level (tools/check_clr_probes.sh).
// ---------------------------------------------------------------------------

TEST(EmitScript, CallWithZeroArgsBuildsEmptyArray) {
    MemoryManager mm;
    // `foo` is never declared — LoxGlobals.Get throws at run time (late
    // binding), but this pass only lowers text, it never executes the
    // program, so an undefined callee is a fine probe for CALL's own shape.
    DecodedFunction fn = decodeScript("foo();", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    // argCount == 0: the callee is already the sole, topmost evaluation-
    // stack value, so the empty array builds directly on top of it — no
    // spill, no scratch slot at all.
    EXPECT_NE(j.find("ldc.i4.0\n"
                     "    newarr [System.Runtime]System.Object\n"
                     "    call object [LoxRuntime]Lox.LoxOps::Call(object, "
                     "object[])\n"),
              std::string::npos)
        << j;
    // globals(1) + frame slots [callee](1) + scratch(1); no call-arg
    // scratch reserved for a zero-argument CALL.
    EXPECT_NE(j.find(".locals init (object, object, object)\n"),
              std::string::npos)
        << j;
}

TEST(EmitScript, CallWithArgsSpillsToScratchSlotsAndBuildsArray) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript("foo(1, 2, 3);", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    // globals(1) + frame slots [callee](1) + scratch(1) + the widest CALL's
    // own spill area (1 callee-scratch slot + 3 argument slots).
    EXPECT_NE(j.find(".locals init (object, object, object, object, object, "
                     "object, object)\n"),
              std::string::npos)
        << j;

    // P7: the values are already on the stack in push order
    // [callee, arg1, arg2, arg3], topmost first — so the topmost (arg3) is
    // spilled first, then arg2, then arg1, then the callee underneath them
    // all.
    EXPECT_NE(j.find("stloc 6\n"
                     "    stloc 5\n"
                     "    stloc 4\n"
                     "    stloc 3\n"),
              std::string::npos)
        << j;
    EXPECT_NE(j.find("ldloc 3\n"
                     "    ldc.i4.3\n"
                     "    newarr [System.Runtime]System.Object\n"),
              std::string::npos)
        << j;
    EXPECT_EQ(countOccurrences(j, "stelem.ref"), 3);
    EXPECT_EQ(countOccurrences(j, "dup"), 3);
    EXPECT_NE(j.find("call object [LoxRuntime]Lox.LoxOps::Call(object, "
                     "object[])"),
              std::string::npos)
        << j;
}

// BUILD_LIST/GET_INDEX/SET_INDEX (this node pulls these three opcodes
// forward from the aggregates scope they conceptually belong to — see
// emitBuildList's own note): V1_fresh_cell and V3_loopvar each build a
// list of the closures under test and read it back by index, so this
// node's own checkpoint needs list and index support to run at all.

TEST(EmitScript, BuildListOfZeroElementsBuildsDirectly) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript("var e = [];", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    EXPECT_NE(j.find("ldc.i4.0\n"
                     "    newarr [System.Runtime]System.Object\n"
                     "    call class [LoxRuntime]Lox.LoxList "
                     "[LoxRuntime]Lox.LoxOps::BuildList(object[])\n"),
              std::string::npos)
        << j;
    expectEveryBranchTargetIsLabeled(j);
}

TEST(EmitScript, BuildListSpillsToScratchSlotsAndBuildsArray) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript("var xs = [1, 2, 3];", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    // globals(1) + frame slots [xs](1) + scratch(1) + CALL's own unused
    // callee scratch(1) + the widest BUILD_LIST/CALL's own 3-element spill
    // area, so argScratchBase is baseSlot(1) + maxLocalCount(1) + scratch(1)
    // + callee(1) = 4.
    //
    // P7, same shuffle CALL's own argument array needs: the elements are
    // already on the stack in push order [e0, e1, e2], topmost (e2) first,
    // so the topmost is spilled first.
    EXPECT_NE(j.find("stloc 6\n"
                     "    stloc 5\n"
                     "    stloc 4\n"),
              std::string::npos)
        << j;
    EXPECT_NE(j.find("ldc.i4.3\n"
                     "    newarr [System.Runtime]System.Object\n"
                     "    dup\n"
                     "    ldc.i4.0\n"
                     "    ldloc 4\n"
                     "    stelem.ref\n"),
              std::string::npos)
        << j;
    EXPECT_NE(j.find("call class [LoxRuntime]Lox.LoxList "
                     "[LoxRuntime]Lox.LoxOps::BuildList(object[])\n"),
              std::string::npos)
        << j;
    EXPECT_EQ(countOccurrences(j, "stelem.ref"), 3);
    EXPECT_EQ(countOccurrences(j, "dup"), 3);
    expectEveryBranchTargetIsLabeled(j);
}

TEST(EmitScript, GetIndexAndSetIndexAreOneCallEach) {
    // vm.cpp's own operand order already matches LoxOps's parameter order
    // (emitGetIndex/emitSetIndex's own note), so neither needs a shuffle —
    // unlike SET_LOCAL/SET_GLOBAL/SET_UPVALUE, this is not a P2 peek.
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("var xs = [1]; print xs[0]; xs[0] = 9;", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    EXPECT_NE(j.find("call object [LoxRuntime]Lox.LoxOps::GetIndex(object, "
                     "object)\n"),
              std::string::npos)
        << j;
    EXPECT_NE(j.find("call object [LoxRuntime]Lox.LoxOps::SetIndex(object, "
                     "object, object)\n"
                     "    pop\n"),
              std::string::npos)
        << j;
    expectEveryBranchTargetIsLabeled(j);
}

// ---------------------------------------------------------------------------
// Classes, methods, and super (this node)
// ---------------------------------------------------------------------------

TEST(EmitScript, ClassOpcodeConstructsWithNullSuperclass) {
    // CLASS builds with superclass=null; INHERIT (below), not CLASS, fills
    // it in on a class that has one. CIL's `newobj` pops its constructor
    // arguments and pushes the fresh reference itself — unlike the JVM
    // backend's own `new;dup;...;invokespecial` idiom, no `dup` is needed
    // to keep an extra reference around.
    MemoryManager mm;
    DecodedFunction fn = decodeScript("class Foo {}", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    EXPECT_NE(j.find("ldstr " + clr::ilasmStringLiteral("Foo") +
                     "\n"
                     "    ldnull\n"
                     "    newobj instance void [LoxRuntime]Lox.LoxClass::"
                     ".ctor(string, class [LoxRuntime]Lox.LoxClass)\n"),
              std::string::npos)
        << j;
    expectEveryBranchTargetIsLabeled(j);
}

TEST(EmitProgram, DefineMethodDupsClassAndCastsBothOperands) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript("class Foo { bar() { return 1; } }\n"
                                      "var f = Foo();\n"
                                      "print f.bar();\n",
                                      mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::string il = clr::emitProgram(fn, tree, "LoxMain");

    // DEFINE_METHOD 'bar': `[cls,fn] -> [cls]` (P2) — `dup` keeps the class
    // value for the class body's own trailing POP; both operands need an
    // explicit `castclass`, since LoxOps.DefineMethod takes concrete types.
    EXPECT_NE(il.find("dup\n"
                      "    castclass [LoxRuntime]Lox.LoxClass\n"
                      "    ldstr " +
                      clr::ilasmStringLiteral("bar") +
                      "\n"
                      "    ldloc 2\n"
                      "    castclass [LoxRuntime]Lox.LoxClosure\n"
                      "    call void [LoxRuntime]Lox.LoxOps::DefineMethod("
                      "class [LoxRuntime]Lox.LoxClass, string, class "
                      "[LoxRuntime]Lox.LoxClosure)\n"),
              std::string::npos)
        << il;
    // INVOKE 'bar' 0: argCount==0 needs no reshuffle at all, same as
    // emitCall's own argCount==0 path — the receiver is already the sole,
    // topmost value.
    EXPECT_NE(il.find("ldstr " + clr::ilasmStringLiteral("bar") +
                      "\n"
                      "    ldc.i4.0\n"
                      "    newarr [System.Runtime]System.Object\n"
                      "    call object [LoxRuntime]Lox.LoxOps::Invoke(object, "
                      "string, object[])\n"),
              std::string::npos)
        << il;
    expectEveryBranchTargetIsLabeled(il);
}

TEST(EmitProgram, GetAndSetPropertyPeekCorrectly) {
    // notes/translation-probes/09_class.lox verbatim.
    MemoryManager mm;
    DecodedFunction fn = decodeScript("class C {\n"
                                      "  init(x) { this.x = x; }\n"
                                      "  get() { return this.x; }\n"
                                      "}\n"
                                      "var c = C(5);\n"
                                      "print c.get();\n",
                                      mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::string il = clr::emitProgram(fn, tree, "LoxMain");

    // SET_PROPERTY 'x': `[obj,v] -> [v]` (P2 shuffle) — `v` spills to the
    // scratch slot while the constant name is pushed between receiver and
    // value; `init`'s own implicit `return this;` (`ldloc 1`) reuses the
    // same slot right afterward.
    EXPECT_NE(il.find("stloc 3\n"
                      "    ldstr " +
                      clr::ilasmStringLiteral("x") +
                      "\n"
                      "    ldloc 3\n"
                      "    call object [LoxRuntime]Lox.LoxOps::"
                      "SetProperty(object, string, object)\n"
                      "    pop\n"
                      "    ldloc 1\n"
                      "    ret\n"),
              std::string::npos)
        << il;
    // GET_PROPERTY 'x': the receiver (`this`, slot 1) is already loaded;
    // only the constant name needs pushing before the call.
    EXPECT_NE(il.find("ldstr " + clr::ilasmStringLiteral("x") +
                      "\n"
                      "    call object [LoxRuntime]Lox.LoxOps::GetProperty("
                      "object, string)\n"
                      "    ret\n"),
              std::string::npos)
        << il;
    expectEveryBranchTargetIsLabeled(il);
}

TEST(EmitProgram, InvokeWithArgsSpillsToScratchSlotsAndBuildsArray) {
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("class C { add(a, b) { return a + b; } }\n"
                     "print C().add(1, 2);\n",
                     mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::string il = clr::emitProgram(fn, tree, "LoxMain");

    // Same spill shape as CALL (emitCall): the receiver and both args are
    // already loose on the stack, so they spill to scratch slots (in
    // reverse pop order) before the array builds on top of the reloaded
    // receiver.
    EXPECT_NE(il.find("stloc 5\n"
                      "    stloc 4\n"
                      "    stloc 3\n"
                      "    ldloc 3\n"
                      "    ldstr " +
                      clr::ilasmStringLiteral("add") +
                      "\n"
                      "    ldc.i4.2\n"
                      "    newarr [System.Runtime]System.Object\n"
                      "    dup\n"
                      "    ldc.i4.0\n"
                      "    ldloc 4\n"
                      "    stelem.ref\n"
                      "    dup\n"
                      "    ldc.i4.1\n"
                      "    ldloc 5\n"
                      "    stelem.ref\n"
                      "    call object [LoxRuntime]Lox.LoxOps::Invoke("
                      "object, string, object[])\n"),
              std::string::npos)
        << il;
    expectEveryBranchTargetIsLabeled(il);
}

TEST(EmitProgram, InheritLoadsSuperclassFromInvisibleVarNotStack) {
    // notes/translation-probes/10_super.lox verbatim.
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("class A { greet() { return 1; } }\n"
                     "class B < A { greet() { return super.greet() + 1; } }\n"
                     "print B().greet();\n",
                     mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::string il = clr::emitProgram(fn, tree, "LoxMain");

    // compiler.cpp's fixed shape (namedVariable(superclass); beginScope();
    // addLocal(super); ...; namedVariable(className); INHERIT) means the
    // superclass value is ALWAYS already the "super" invisible var by the
    // time INHERIT runs, not a live evaluation-stack temp — so this loads
    // it back rather than assuming it still sits beneath the subclass on
    // the physical stack. `super` is also captured by B's own `greet`
    // CLOSURE later in this same chunk (for `super.greet()`), so by the
    // time INHERIT reads it back, it is already wrapped in a ref-cell — the
    // same `isinst object[]` test every other captured-slot read shares.
    EXPECT_NE(il.find("isinst object[]\n"
                      "    brfalse Ccapgr"),
              std::string::npos)
        << il;
    EXPECT_NE(il.find("call void [LoxRuntime]Lox.LoxOps::InheritInto(object, "
                      "object)\n"),
              std::string::npos)
        << il;
    expectEveryBranchTargetIsLabeled(il);
}

TEST(EmitProgram, SuperInvokeZeroArgsUsesTwoScratchSlots) {
    // notes/translation-probes/10_super.lox's own method body: CIL has no
    // `swap` (unlike the JVM backend's own emitSuperInvoke, which reduces
    // this shape to one `swap` plus a single scratch slot), so both self
    // and the superclass need their own scratch slot here.
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("class A { greet() { return 1; } }\n"
                     "class B < A { greet() { return super.greet() + 1; } }\n"
                     "print B().greet();\n",
                     mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::string il = clr::emitProgram(fn, tree, "LoxMain");

    EXPECT_NE(il.find("stloc 2\n"
                      "    stloc 3\n"
                      "    ldloc 2\n"
                      "    ldstr " +
                      clr::ilasmStringLiteral("greet") +
                      "\n"
                      "    ldloc 3\n"
                      "    ldc.i4.0\n"
                      "    newarr [System.Runtime]System.Object\n"
                      "    call object [LoxRuntime]Lox.LoxOps::"
                      "SuperInvoke(object, string, object, object[])\n"),
              std::string::npos)
        << il;
    expectEveryBranchTargetIsLabeled(il);
}

TEST(EmitProgram, SuperInvokeWithArgsSpillsThreeDistinctScratchSlots) {
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("class A { add(a, b) { return a + b; } }\n"
                     "class B < A { add(a, b) { return super.add(a, b); } }\n"
                     "print B().add(1, 2);\n",
                     mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::string il = clr::emitProgram(fn, tree, "LoxMain");

    // `e.scratchSlot` holds the superclass, `e.calleeScratchSlot` holds
    // self, `e.argScratchBase` holds the spilled arguments — three
    // DISTINCT slots, since one instruction runs to completion before the
    // next starts.
    EXPECT_NE(il.find("call object [LoxRuntime]Lox.LoxOps::SuperInvoke("
                      "object, string, object, object[])\n"),
              std::string::npos)
        << il;
    expectEveryBranchTargetIsLabeled(il);
}

TEST(EmitProgram, GetSuperBindsMethodAsValue) {
    // notes/translation-probes/17_super_value.lox verbatim.
    MemoryManager mm;
    DecodedFunction fn = decodeScript(
        "class A { greet() { return 1; } }\n"
        "class B < A { greet() { var f = super.greet; return f(); } }\n"
        "print B().greet();\n",
        mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::string il = clr::emitProgram(fn, tree, "LoxMain");

    EXPECT_NE(il.find("ldstr " + clr::ilasmStringLiteral("greet") +
                      "\n"
                      "    ldloc 4\n"
                      "    call object [LoxRuntime]Lox.LoxOps::GetSuper("
                      "object, string, object)\n"),
              std::string::npos)
        << il;
    expectEveryBranchTargetIsLabeled(il);
}

TEST(EmitScript, InstanceofChecksGlobalsByName) {
    // A class pattern is INSTANCEOF's only source (compiler.cpp) — there is
    // no standalone `is` operator to reach it any other way.
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("class A {}\n"
                     "var a = A();\n"
                     "print match a { case A => true case _ => false };\n",
                     mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    // vm.cpp looks the class up BY NAME in globals, not from a
    // constant-pool class reference — this pass only supplies the
    // already-open globals receiver and the constant name.
    EXPECT_NE(j.find("ldloc 0\n"
                     "    ldstr " +
                     clr::ilasmStringLiteral("A") +
                     "\n"
                     "    call bool [LoxRuntime]Lox.LoxOps::InstanceOf("
                     "object, class [LoxRuntime]Lox.LoxGlobals, "
                     "string)\n"
                     "    box [System.Runtime]System.Boolean\n"),
              std::string::npos)
        << j;
    expectEveryBranchTargetIsLabeled(j);
}

TEST(EmitScript, MatchErrorBuildsThenThrows) {
    // notes/translation-probes/33_class_pattern_match_error.lox's own
    // shape: a match whose arms are all class patterns, no unguarded
    // catch-all.
    MemoryManager mm;
    DecodedFunction fn = decodeScript("class A {}\n"
                                      "class B {}\n"
                                      "print match A() { case B => 1 };\n",
                                      mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    // LoxOps.MatchError BUILDS the error rather than throwing it, so the
    // call leaves a real value on the stack and `throw` — a genuine
    // terminal instruction — ends the block, the same way the JVM
    // backend's own athrow does.
    EXPECT_NE(j.find("call class [LoxRuntime]Lox.LoxError "
                     "[LoxRuntime]Lox.LoxOps::MatchError()\n"
                     "    throw\n"),
              std::string::npos)
        << j;
    expectEveryBranchTargetIsLabeled(j);
}

// The consumed-match case (this node's own checkpoint,
// notes/translation-probes/32_match_consumed_result.lox): a match
// expression's own closing POP retires only the synthetic "subject"
// local, exposing the arm's own result local as the new top with no
// separate value ever pushed — so PRINT and DEFINE_GLOBAL need the same
// zero-depth fold check the peek family (SET_LOCAL/SET_GLOBAL/
// JUMP_IF_FALSE/RETURN) already has.

TEST(EmitScript, DefineGlobalOfAFoldedMatchResultLoadsInsteadOfAssumingATemp) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript("var n = match 1 { case _ => 5 };", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    // The match's own arm stores its result in its own named local (slot
    // 2); DEFINE_GLOBAL reloads that exact slot (through the scratch slot
    // `emitGlobalsCall`'s own shuffle uses) rather than assuming a value
    // already sits on the CIL stack — which would pop whatever
    // coincidentally sits there instead.
    EXPECT_NE(j.find("ldloc 2\n"
                     "    stloc 4\n"
                     "    ldloc 0\n"
                     "    ldstr " +
                     clr::ilasmStringLiteral("n") +
                     "\n"
                     "    ldloc 4\n"
                     "    call instance void [LoxRuntime]Lox.LoxGlobals::"
                     "Define(string, object)\n"),
              std::string::npos)
        << j;
    expectEveryBranchTargetIsLabeled(j);
}

TEST(EmitScript, PrintOfAFoldedMatchResultLoadsInsteadOfAssumingATemp) {
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("print match 1 { case 1 => \"a\" case _ => \"b\" };", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = clr::emitScript(fn, analysis, "LoxMain");

    // Both arms store their result into the same named local (the fold);
    // the final merge label reloads that local right before PRINT, which
    // would otherwise underflow the CIL evaluation stack (nothing was ever
    // pushed there for a genuine consumer to read).
    EXPECT_NE(j.find("ldloc 2\n"
                     "    call void [LoxRuntime]Lox.LoxOps::Print(object)\n"),
              std::string::npos)
        << j;
    expectEveryBranchTargetIsLabeled(j);
}

TEST(EmitProgram, ZeroUpvalueClosureConstructsGeneratedClass) {
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("fun add(a, b) { return a + b; } print add(1, 2);", mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::string il = clr::emitProgram(fn, tree, "LoxMain");

    ASSERT_EQ(countOccurrences(il, ".assembly extern LoxRuntime"), 1);
    ASSERT_EQ(countOccurrences(il, ".class public auto ansi"), 2);
    EXPECT_NE(il.find(".class public auto ansi LoxMain"), std::string::npos)
        << il;
    EXPECT_NE(il.find(".class public auto ansi LoxFn$0"), std::string::npos)
        << il;

    // Zero upvalues (this node's own top-of-file note; a captured one is a
    // later node's wiring): an empty object[][], then one `newobj` builds
    // and constructs the generated class in a single instruction.
    EXPECT_NE(il.find("ldc.i4.0\n"
                      "    newarr object[]\n"
                      "    newobj instance void LoxFn$0::.ctor(object[][])\n"),
              std::string::npos)
        << il;

    EXPECT_NE(il.find("LoxFn$0 extends [LoxRuntime]Lox.LoxClosure"),
              std::string::npos)
        << il;
    EXPECT_NE(il.find(".ctor(object[][] upvalues)"), std::string::npos) << il;
    // <ctor>'s own literals: this function's compile-time name and arity.
    EXPECT_NE(il.find("ldstr " + clr::ilasmStringLiteral("add")),
              std::string::npos)
        << il;
    EXPECT_NE(il.find("call instance void [LoxRuntime]Lox.LoxClosure::.ctor("
                      "string, int32, object[][])"),
              std::string::npos)
        << il;
    EXPECT_NE(il.find("Invoke(object self, object[] args)"), std::string::npos)
        << il;

    // Argument prologue (P5): `self` (arg 1) copied into slot 1 (`a`'s
    // Lox-frame-slot-0 mirror, baseSlot=1 for either role), then
    // args[0]/args[1] unpacked into slots 2/3 (`a`, `b`).
    EXPECT_NE(il.find("ldarg.1\n    stloc 1\n"), std::string::npos) << il;
    EXPECT_NE(il.find("ldarg.2\n"
                      "    ldc.i4.0\n"
                      "    ldelem.ref\n"
                      "    stloc 2\n"),
              std::string::npos)
        << il;
    EXPECT_NE(il.find("ldarg.2\n"
                      "    ldc.i4.1\n"
                      "    ldelem.ref\n"
                      "    stloc 3\n"),
              std::string::npos)
        << il;
    // RETURN's function role: exactly the one return value on the
    // evaluation stack, then `ret` — never the script's `pop; ret`.
    // LoxMain's script `ret`, LoxFn$0's own <ctor> `ret`, and Invoke's own.
    EXPECT_EQ(countOccurrences(il, "\n    ret\n"), 3);
    expectEveryBranchTargetIsLabeled(il);
}

TEST(EmitProgram, SiblingFunctionsGetSequentialClassNames) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript("fun a() { return 1; }\n"
                                      "fun b() { return 2; }\n"
                                      "print a();\n"
                                      "print b();\n",
                                      mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::string il = clr::emitProgram(fn, tree, "LoxMain");

    // Deterministic naming (clr_emitter.h): one pre-order counter over the
    // whole tree, not per parent — `a` and `b` are siblings, so they draw 0
    // and 1 in declaration order.
    ASSERT_EQ(countOccurrences(il, ".class public auto ansi"), 3);
    EXPECT_NE(il.find(".class public auto ansi LoxMain"), std::string::npos)
        << il;
    EXPECT_NE(il.find(".class public auto ansi LoxFn$0"), std::string::npos)
        << il;
    EXPECT_NE(il.find(".class public auto ansi LoxFn$1"), std::string::npos)
        << il;
    expectEveryBranchTargetIsLabeled(il);
}

// Closures and upvalues (this node, the bug gate). The real, end-to-end
// proof that a captured local behaves correctly (V1_fresh_cell, V2_shared,
// V3_loopvar, V4_mutate_through_upvalue, V5/V6_self_recursive_closure,
// 06_shared_upvalue) is tools/check_clr_probes.sh, run inside the
// dev-managed container — a plain unit test cannot assemble+run ilasm/
// dotnet. What follows checks the structural shape this pass promises: the
// idempotent seed at a CLOSURE that captures a local, GET/SET_UPVALUE's own
// lowering, and the isLocal=false pass-through for a grandparent's upvalue
// — mirroring test_jvm_emit.cpp's own coverage of the same shapes.

TEST(EmitProgram, SingleUpvalueClosureSeedsAndWiresTheCell) {
    // outer's own slot 1 is `x`. slotForLocal(1) with baseSlot=1 is 2.
    MemoryManager mm;
    DecodedFunction fn = decodeScript("fun outer() {\n"
                                      "  var x = 0;\n"
                                      "  fun get() { return x; }\n"
                                      "  return get;\n"
                                      "}\n",
                                      mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::string il = clr::emitProgram(fn, tree, "LoxMain");

    ASSERT_EQ(countOccurrences(il, ".class public auto ansi"), 3);
    std::size_t outerStart = il.find(".class public auto ansi LoxFn$0");
    ASSERT_NE(outerStart, std::string::npos) << il;
    std::size_t getStart = il.find(".class public auto ansi LoxFn$1");
    ASSERT_NE(getStart, std::string::npos) << il;
    std::string outer = il.substr(outerStart, getStart - outerStart);
    std::string get = il.substr(getStart);

    // The idempotent seed (ensureCapturedCell): check, seed only if not
    // already a cell, then read the (now guaranteed) cell for wiring.
    EXPECT_NE(outer.find("ldloc 2\n"
                         "    isinst object[]\n"
                         "    brtrue Ccapok3_0\n"
                         "    ldc.i4.1\n"
                         "    newarr [System.Runtime]System.Object\n"
                         "    dup\n"
                         "    ldc.i4.0\n"
                         "    ldloc 2\n"
                         "    stelem.ref\n"
                         "    stloc 2\n"
                         "Ccapok3_0:\n"),
              std::string::npos)
        << outer;
    EXPECT_NE(
        outer.find("newarr object[]\n"
                   "    dup\n"
                   "    ldc.i4.0\n"
                   "    ldloc 2\n"
                   "    castclass object[]\n"
                   "    stelem.ref\n"
                   "    newobj instance void LoxFn$1::.ctor(object[][])\n"),
        std::string::npos)
        << outer;
    expectEveryBranchTargetIsLabeled(outer);

    // GET_UPVALUE 0: upvals[0][0].
    EXPECT_NE(get.find("ldarg.0\n"
                       "    ldfld object[][] [LoxRuntime]Lox.LoxClosure::"
                       "Upvalues\n"
                       "    ldc.i4.0\n"
                       "    ldelem.ref\n"
                       "    ldc.i4.0\n"
                       "    ldelem.ref\n"
                       "    ret\n"),
              std::string::npos)
        << get;
    expectEveryBranchTargetIsLabeled(get);
}

TEST(EmitProgram, TwoClosuresShareOneCaptureCell) {
    // 06_shared_upvalue: get and set both capture x. Each CLOSURE gets its
    // OWN idempotent check (distinct labels, tied to its own offset —
    // captureLabel), because ensureCapturedCell cannot assume the other one
    // ran first on every path (this node's own hazard: two closures
    // sharing a cell is not always sequential — an if/else can capture the
    // same outer on mutually exclusive arms). Here both run on the SAME
    // straight-line path, so the second one's check is a real no-op at
    // runtime, but the CIL still carries both checks.
    MemoryManager mm;
    DecodedFunction fn = decodeScript("fun outer() {\n"
                                      "  var x = 0;\n"
                                      "  fun get() { return x; }\n"
                                      "  fun set(v) { x = v; }\n"
                                      "  return get;\n"
                                      "}\n",
                                      mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::string il = clr::emitProgram(fn, tree, "LoxMain");

    ASSERT_EQ(countOccurrences(il, ".class public auto ansi"), 4);
    std::size_t outerStart = il.find(".class public auto ansi LoxFn$0");
    std::size_t getStart = il.find(".class public auto ansi LoxFn$1");
    std::size_t setStart = il.find(".class public auto ansi LoxFn$2");
    ASSERT_NE(outerStart, std::string::npos) << il;
    ASSERT_NE(getStart, std::string::npos) << il;
    ASSERT_NE(setStart, std::string::npos) << il;
    std::string outer = il.substr(outerStart, getStart - outerStart);
    std::string set = il.substr(setStart);

    EXPECT_NE(outer.find("Ccapok3_0:"), std::string::npos) << outer;
    EXPECT_NE(outer.find("Ccapok8_0:"), std::string::npos) << outer;
    // Both CLOSUREs read the SAME slot (2) for their cell — one shared cell.
    EXPECT_EQ(countOccurrences(outer, "ldloc 2\n"
                                      "    castclass object[]\n"
                                      "    stelem.ref\n"),
              2);
    expectEveryBranchTargetIsLabeled(outer);

    // set(v): SET_UPVALUE 0 writes upvals[0][0], fused with its own
    // trailing POP (the assignment is a bare statement) — no leftover
    // reload of the spilled value.
    EXPECT_NE(set.find("ldfld object[][] [LoxRuntime]Lox.LoxClosure::"
                       "Upvalues\n"
                       "    ldc.i4.0\n"
                       "    ldelem.ref\n"
                       "    ldc.i4.0\n"),
              std::string::npos)
        << set;
    EXPECT_NE(set.find("stelem.ref\n"
                       "    ldnull\n"
                       "    ret\n"),
              std::string::npos)
        << set;
    expectEveryBranchTargetIsLabeled(set);
}

// emitCapturedGetLocal and emitCapturedStore each guard with their own
// isinst object[] test (this file's own top-of-file note), but neither
// closure test above reaches them directly: SingleUpvalueClosureSeedsAnd
// WiresTheCell only reaches GET_UPVALUE (inside get's own body) and
// TwoClosuresShareOneCaptureCell only reaches SET_UPVALUE (inside set's
// own body). This pins the shape of both functions where they actually
// run: outer's OWN GET_LOCAL/SET_LOCAL of its captured `x`, after the
// CLOSURE that captures it.
TEST(EmitProgram, CapturedLocalGetAndSetLocalGuardWithIsinstAfterCapture) {
    // outer's own slot 1 is `x`. slotForLocal(1) with baseSlot=1 is 2.
    MemoryManager mm;
    DecodedFunction fn = decodeScript("fun outer() {\n"
                                      "  var x = 0;\n"
                                      "  fun get() { return x; }\n"
                                      "  x = x + 1;\n"
                                      "  print x;\n"
                                      "  return get;\n"
                                      "}\n",
                                      mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::string il = clr::emitProgram(fn, tree, "LoxMain");

    ASSERT_EQ(countOccurrences(il, ".class public auto ansi"), 3);
    std::size_t outerStart = il.find(".class public auto ansi LoxFn$0");
    std::size_t getStart = il.find(".class public auto ansi LoxFn$1");
    ASSERT_NE(outerStart, std::string::npos) << il;
    ASSERT_NE(getStart, std::string::npos) << il;
    std::string outer = il.substr(outerStart, getStart - outerStart);

    // emitCapturedGetLocal's own shape, the read side of `x + 1`: isinst,
    // brfalse to its own raw label, castclass+ldelem.ref on the cell
    // branch.
    EXPECT_NE(outer.find("isinst object[]\n"
                         "    brfalse Ccapgr"),
              std::string::npos)
        << outer;
    EXPECT_NE(outer.find("castclass object[]\n"
                         "    ldc.i4.0\n"
                         "    ldelem.ref\n"
                         "    br Ccapge"),
              std::string::npos)
        << outer;

    // emitCapturedStore's own shape, the write side of `x = x + 1`: the
    // value spills to scratch first (P2, emitCapturedStore's own note),
    // then the same isinst guard picks stelem.ref-into-the-cell or a bare
    // stloc.
    EXPECT_NE(outer.find("isinst object[]\n"
                         "    brfalse Ccapsr"),
              std::string::npos)
        << outer;
    EXPECT_NE(outer.find("castclass object[]\n"
                         "    ldc.i4.0\n"
                         "    ldloc "),
              std::string::npos)
        << outer;
    EXPECT_NE(outer.find("stelem.ref\n"
                         "    br Ccapse"),
              std::string::npos)
        << outer;
    expectEveryBranchTargetIsLabeled(outer);
}

TEST(EmitProgram, NestedClosureCopiesGrandparentUpvalue) {
    // c captures x, which is a's local but b's own upvalue (isLocal=false):
    // b's own CLOSURE-of-c wiring copies its OWN upvals[0] reference
    // straight through, with no seed check at all — see emitClosure's own
    // note. Only a's CLOSURE-of-b needs ensureCapturedCell.
    MemoryManager mm;
    DecodedFunction fn = decodeScript("fun a() {\n"
                                      "  var x = 1;\n"
                                      "  fun b() {\n"
                                      "    fun c() { return x; }\n"
                                      "    return c;\n"
                                      "  }\n"
                                      "  return b;\n"
                                      "}\n",
                                      mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::string il = clr::emitProgram(fn, tree, "LoxMain");

    ASSERT_EQ(countOccurrences(il, ".class public auto ansi"), 4);
    std::size_t bStart = il.find(".class public auto ansi LoxFn$1");
    std::size_t cStart = il.find(".class public auto ansi LoxFn$2");
    ASSERT_NE(bStart, std::string::npos) << il;
    ASSERT_NE(cStart, std::string::npos) << il;
    std::string bFn = il.substr(bStart, cStart - bStart);

    EXPECT_EQ(bFn.find("isinst"), std::string::npos) << bFn;
    EXPECT_NE(bFn.find("dup\n"
                       "    ldc.i4.0\n"
                       "    ldarg.0\n"
                       "    ldfld object[][] [LoxRuntime]Lox.LoxClosure::"
                       "Upvalues\n"
                       "    ldc.i4.0\n"
                       "    ldelem.ref\n"
                       "    stelem.ref\n"),
              std::string::npos)
        << bFn;
    expectEveryBranchTargetIsLabeled(bFn);
}

// A local `fun` that captures itself needs seedSelfCaptureCell to bind a
// fresh cell to its own slot before the closure's array-build loop reads
// that slot. Reverting that seed locally and rerunning this test confirms
// it FAILS first: without the seed, the very first "stloc 2" this test
// looks for does not exist before the closure's own array-build read of
// that same slot (it is instead the stelem.ref-redirect after
// construction), so `firstReadPos` would sit ahead of a missing/later
// `seedPos`, or `seedPos` would not be found at all.
TEST(EmitProgram, SelfRecursiveClosureSeedsCellBeforeFirstRead) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript("fun outer() {\n"
                                      "  fun f() { f(); }\n"
                                      "  return f;\n"
                                      "}\n",
                                      mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::string il = clr::emitProgram(fn, tree, "LoxMain");

    ASSERT_EQ(countOccurrences(il, ".class public auto ansi"), 3);
    std::size_t outerStart = il.find(".class public auto ansi LoxFn$0");
    std::size_t fStart = il.find(".class public auto ansi LoxFn$1");
    ASSERT_NE(outerStart, std::string::npos) << il;
    std::string outer = il.substr(outerStart, fStart - outerStart);

    // f's own Lox slot 1 (slotForLocal(1) with baseSlot=1) is 2: the seed
    // (`stloc 2`) must precede every later `ldloc 2` that reads it as a
    // cell — the array-build loop, and the redirected store of the closure
    // itself into the cell.
    std::size_t seedPos = outer.find("stloc 2\n");
    ASSERT_NE(seedPos, std::string::npos) << outer;
    std::size_t firstReadPos = outer.find("ldloc 2\n");
    ASSERT_NE(firstReadPos, std::string::npos) << outer;
    EXPECT_LT(seedPos, firstReadPos)
        << "the seed must run before the first read of the self-captured "
           "slot, or a bare local read is uninitialized:\n"
        << outer;
    expectEveryBranchTargetIsLabeled(outer);
}

// RETURN's function-role hazard (node specification): the shared
// abstract-stack analysis can fold the returned value into a named local
// rather than leave it as a genuine evaluation-stack temporary — 33 sites
// in the corpus (bytecode-translation-problems.md), none reachable from
// this node's own opcode set alone (it needs a `match` expression), so
// this hand-builds the shape directly rather than waiting for a later
// node's program to reach it. Mirrors this file's own
// `SetLocalPeekOfNamedLocalLoadsInsteadOfDup` technique, one level deeper
// (a function's own RETURN instead of a script's SET_LOCAL).
TEST(EmitProgram, ReturnOfAFoldedLocalLoadsInsteadOfAssumingATemp) {
    MemoryManager mm;

    DecodedInstruction nil;
    nil.offset = 0;
    nil.op = Op::NIL;
    nil.length = 1;
    DecodedInstruction ret;
    ret.offset = 1;
    ret.op = Op::RETURN;
    ret.length = 1;

    DecodedFunction child;
    child.id = "0.0";
    child.function = mm.create<ObjFunction>();
    child.function->arity = 0;
    child.instructions = {nil, ret};

    FunctionStackAnalysis childAnalysis;
    childAnalysis.functionId = "0.0";
    // The NIL's own push lands directly in local slot 1 (an invisible-var
    // site at this very offset — no separate store instruction), so by
    // RETURN's own offset the evaluation stack already reads back to depth
    // 0: nothing genuine sits above the newly-declared local.
    childAnalysis.before = {StackState{1, 1}, StackState{2, 2}};
    childAnalysis.after = {StackState{2, 2}, StackState{1, 1}};
    childAnalysis.reached = {true, true};
    childAnalysis.invisibleVars = {InvisibleVarSite{0, 1}};

    DecodedFunction root;
    root.id = "0";
    root.function = mm.create<ObjFunction>();
    root.nested = {child};

    DecodedInstruction rootNil;
    rootNil.offset = 0;
    rootNil.op = Op::NIL;
    rootNil.length = 1;
    DecodedInstruction rootRet;
    rootRet.offset = 1;
    rootRet.op = Op::RETURN;
    rootRet.length = 1;
    root.instructions = {rootNil, rootRet};

    FunctionStackAnalysis rootAnalysis;
    rootAnalysis.functionId = "0";
    rootAnalysis.before = {StackState{0, 0}, StackState{1, 0}};
    rootAnalysis.after = {StackState{1, 0}, StackState{0, 0}};
    rootAnalysis.reached = {true, true};

    StackAnalysisTree tree;
    tree.self = rootAnalysis;
    tree.nested = {StackAnalysisTree{childAnalysis, {}}};

    std::string il = clr::emitProgram(root, tree, "LoxMain");

    ASSERT_NE(il.find("LoxFn$0 extends [LoxRuntime]Lox.LoxClosure"),
              std::string::npos)
        << il;
    std::size_t fnStart = il.find("LoxFn$0 extends");
    std::string fn0 = il.substr(fnStart);

    // The fix: load slot 2 (baseSlot=1 + Lox slot 1) explicitly, then
    // return it directly — never a bare `ret` that assumes a physical
    // temporary was already sitting on the evaluation stack.
    EXPECT_NE(fn0.find("ldloc 2\n    ret\n"), std::string::npos) << fn0;
}

TEST(EmitProgram, ReturnWithMoreThanTheReturnValueOnTheStackThrows) {
    // The mirror image of ReturnWithANonEmptyEvaluationStackThrows (script
    // role): a function's own `ret` must find EXACTLY the return value —
    // this hand-built chunk gives it two, so the check must fire rather
    // than silently returning the wrong one of the two.
    MemoryManager mm;

    DecodedInstruction nil0;
    nil0.offset = 0;
    nil0.op = Op::NIL;
    nil0.length = 1;
    DecodedInstruction nil1;
    nil1.offset = 1;
    nil1.op = Op::NIL;
    nil1.length = 1;
    DecodedInstruction ret;
    ret.offset = 2;
    ret.op = Op::RETURN;
    ret.length = 1;

    DecodedFunction child;
    child.id = "0.0";
    child.function = mm.create<ObjFunction>();
    child.instructions = {nil0, nil1, ret};

    FunctionStackAnalysis childAnalysis;
    childAnalysis.functionId = "0.0";
    childAnalysis.before = {StackState{1, 1}, StackState{2, 1},
                            StackState{3, 1}};
    childAnalysis.after = {StackState{2, 1}, StackState{3, 1},
                           StackState{2, 1}};
    childAnalysis.reached = {true, true, true};

    DecodedFunction root;
    root.id = "0";
    root.function = mm.create<ObjFunction>();
    root.nested = {child};
    DecodedInstruction rootNil;
    rootNil.offset = 0;
    rootNil.op = Op::NIL;
    rootNil.length = 1;
    DecodedInstruction rootRet;
    rootRet.offset = 1;
    rootRet.op = Op::RETURN;
    rootRet.length = 1;
    root.instructions = {rootNil, rootRet};

    FunctionStackAnalysis rootAnalysis;
    rootAnalysis.functionId = "0";
    rootAnalysis.before = {StackState{0, 0}, StackState{1, 0}};
    rootAnalysis.after = {StackState{1, 0}, StackState{0, 0}};
    rootAnalysis.reached = {true, true};

    StackAnalysisTree tree;
    tree.self = rootAnalysis;
    tree.nested = {StackAnalysisTree{childAnalysis, {}}};

    try {
        clr::emitProgram(root, tree, "LoxMain");
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("exactly the return value"),
                  std::string::npos)
            << e.what();
    }
}

TEST(EmitProgram, MismatchedNestedChildCountThrowsInsteadOfReadingOutOfRange) {
    // decodeFunctionTree and analyzeStackTree are two independently walked
    // passes over the same ObjFunction tree; emitAll assumes they agree on
    // child count at every node. This hand-builds a root whose decoded
    // tree has one nested function but whose stack-analysis tree has none,
    // the one disagreement shape a real chunk decode/analyze pair never
    // produces, to prove the size check fires instead of the loop reading
    // node.nested[0] out of range.
    MemoryManager mm;

    DecodedFunction child;
    child.id = "0.0";
    child.function = mm.create<ObjFunction>();

    DecodedFunction root;
    root.id = "0";
    root.function = mm.create<ObjFunction>();
    root.nested = {child};
    DecodedInstruction rootNil;
    rootNil.offset = 0;
    rootNil.op = Op::NIL;
    rootNil.length = 1;
    DecodedInstruction rootRet;
    rootRet.offset = 1;
    rootRet.op = Op::RETURN;
    rootRet.length = 1;
    root.instructions = {rootNil, rootRet};

    FunctionStackAnalysis rootAnalysis;
    rootAnalysis.functionId = "0";
    rootAnalysis.before = {StackState{0, 0}, StackState{1, 0}};
    rootAnalysis.after = {StackState{1, 0}, StackState{0, 0}};
    rootAnalysis.reached = {true, true};

    StackAnalysisTree tree;
    tree.self = rootAnalysis;
    // Deliberately empty: root.nested has one entry, tree.nested has none.

    try {
        clr::emitProgram(root, tree, "LoxMain");
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("disagree on child count"),
                  std::string::npos)
            << e.what();
    }
}

// ---------------------------------------------------------------------------
// resolveZeroDepthLocalSlot (zero_depth_local.h): the one, target-
// independent authority both this file and jvm_emitter.cpp call into for
// the depth-0 named-local cross-check (clr_emitter.h's own top-of-file
// note). No program in this node's own opcode set (if/else, and/or, while,
// for, function bodies) makes the two estimates disagree — the scope-exit
// rule retires every local a block declared before control reaches
// outside it, and a function's own body merges through the same if/else/
// while/for shapes as a script chunk, so a disagreement still needs a
// `match` arm (out of this node's scope) — so this exercises the
// cross-check directly rather than asserting it can never be reached
// indirectly.
// ---------------------------------------------------------------------------

TEST(ResolveZeroDepthLocalSlot, AwayFromAMergeReadsLocalCountDirectly) {
    // atCfgMergeLabel == false: the tracker is never consulted, even when
    // it holds a nonsense sentinel (-1, "no invisible-var site has run
    // yet").
    EXPECT_EQ(resolveZeroDepthLocalSlot(/*exactLocalCountMinusOne=*/2,
                                        /*atCfgMergeLabel=*/false,
                                        /*lastInvisibleVarSlot=*/-1,
                                        /*offset=*/9, "clr_emitter"),
              2);
}

TEST(ResolveZeroDepthLocalSlot, AtAMergeAgreementIsSilent) {
    EXPECT_EQ(resolveZeroDepthLocalSlot(/*exactLocalCountMinusOne=*/2,
                                        /*atCfgMergeLabel=*/true,
                                        /*lastInvisibleVarSlot=*/2,
                                        /*offset=*/9, "clr_emitter"),
              2);
}

TEST(ResolveZeroDepthLocalSlot, AtAMergeDisagreementThrows) {
    try {
        resolveZeroDepthLocalSlot(/*exactLocalCountMinusOne=*/2,
                                  /*atCfgMergeLabel=*/true,
                                  /*lastInvisibleVarSlot=*/1, /*offset=*/9,
                                  "clr_emitter");
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        std::string what = e.what();
        EXPECT_NE(what.find("clr_emitter"), std::string::npos) << what;
        EXPECT_NE(what.find("CFG merge"), std::string::npos) << what;
        EXPECT_NE(what.find('9'), std::string::npos) << what;
    }
}

} // namespace
