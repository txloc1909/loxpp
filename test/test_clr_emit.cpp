// test_clr_emit.cpp — CLR straight-line emitter.
//
// Checkpoint (see the node's own specification):
//   tools/loxpp_clr.sh notes/translation-probes/{01,15,18,19,20,21}_*.lox
// must each print stdout identical to build/loxpp on the same file. That
// full assemble-and-run comparison needs ilasm/dotnet
// (tools/check_clr_probes.sh, run inside the dev-managed container); this
// file covers what a plain C++ unit test can check without them: the
// bytearray/bit-pattern literal helpers, and the generated ilasm's
// structural shape (locals, maxstack, fusion, abort-on-unsupported).

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
const std::vector<std::string> kBranchMnemonics = {"br ", "brtrue "};

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

// This node's own opcode set (clr_emitter.h) has no CALL, no CLOSURE, no
// control flow, and no match — so JUMP is one of many opcodes this pass
// must refuse rather than silently mis-lower. A hand-built single-
// instruction chunk drives the refusal directly, the same way
// test_jvm_emit.cpp's own `AbortsOnUnsupportedOpcode` does, rather than
// hunting for a real Lox++ program that reaches an opcode this pass has no
// case for.
TEST(EmitScript, AbortsOnUnsupportedOpcode) {
    // JUMP/JUMP_IF_FALSE/LOOP are no longer unsupported (this node); CALL
    // is the nearest opcode still outside this pass's scope.
    MemoryManager mm;
    DecodedInstruction call;
    call.offset = 0;
    call.op = Op::CALL;
    call.length = 2;
    call.byteOperand = 0;

    DecodedFunction fn;
    fn.id = "0";
    fn.function = mm.create<ObjFunction>();
    fn.instructions = {call};

    FunctionStackAnalysis analysis;
    analysis.functionId = "0";
    analysis.before = {StackState{0, 0}};
    analysis.after = {StackState{0, 0}};
    analysis.reached = {true};

    try {
        clr::emitScript(fn, analysis, "LoxMain");
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("CALL"), std::string::npos)
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
    // empty stack would trip the R1 depth safety net at emit time (proven
    // by temporarily disabling the fix); the actual fix reloads a fresh
    // copy through `loadNamedLocalAtZeroDepth` instead.
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
// fix, proven to matter by temporarily disabling it: the R1 depth safety
// net then tripped with "simulated stack depth 0 disagrees with analysis
// depth 1" on 02_if_else).
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
// resolveZeroDepthLocalSlot (zero_depth_local.h): the one, target-
// independent authority both this file and jvm_emitter.cpp call into for
// the depth-0 named-local cross-check (clr_emitter.h's own top-of-file
// note). No program in this node's own opcode set (if/else, and/or, while,
// for) makes the two estimates disagree — the scope-exit rule retires
// every local a block declared before control reaches outside it, so a
// disagreement needs a function frame or a `match` arm (neither in this
// node's scope) — so this exercises the cross-check directly rather than
// asserting it can never be reached indirectly.
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
