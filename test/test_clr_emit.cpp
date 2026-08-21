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
#include "compiler.h"
#include "memory_manager.h"
#include "object.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
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
    MemoryManager mm;
    DecodedInstruction jump;
    jump.offset = 0;
    jump.op = Op::JUMP;
    jump.length = 3;
    jump.jumpTarget = 0;

    DecodedFunction fn;
    fn.id = "0";
    fn.function = mm.create<ObjFunction>();
    fn.instructions = {jump};

    FunctionStackAnalysis analysis;
    analysis.functionId = "0";
    analysis.before = {StackState{0, 0}};
    analysis.after = {StackState{0, 0}};
    analysis.reached = {true};

    try {
        clr::emitScript(fn, analysis, "LoxMain");
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("JUMP"), std::string::npos)
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

} // namespace
