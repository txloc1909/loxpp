// test_jvm_emit.cpp — JVM straight-line emitter (node N4).
//
// Checkpoint (notes/backend-implementation-dag.md, node N4):
//   tools/loxpp_jvm.sh notes/translation-probes/01_assign_local.lox
//   tools/loxpp_jvm.sh notes/translation-probes/15_nested_arith.lox
// must each print stdout identical to build/loxpp on the same file. That
// full assemble-and-run comparison needs jasmin/java
// (tools/check_jvm_probes.sh, run inside the dev-managed container); this file
// covers what a plain C++ unit test can check without them: the
// escaping/formatting helpers, the generated Jasmin's structural shape (limits,
// fusion, abort-on-unsupported).

#include "backend/abstract_stack.h"
#include "backend/chunk_decoder.h"
#include "backend/jvm_emitter.h"
#include "compiler.h"
#include "memory_manager.h"
#include "object.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace {

DecodedFunction decodeScript(const std::string& source, MemoryManager& mm) {
    ObjFunction* script = compile(source, &mm);
    if (script == nullptr) {
        throw std::runtime_error("compilation failed");
    }
    return decodeFunctionTree(script);
}

// Counts non-overlapping occurrences of `needle` in `haystack`.
int countOccurrences(const std::string& haystack, const std::string& needle) {
    int count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        count++;
        pos += needle.size();
    }
    return count;
}

} // namespace

// ---------------------------------------------------------------------------
// escapeJasminString
// ---------------------------------------------------------------------------

TEST(EscapeJasminString, PlainAsciiPassesThrough) {
    EXPECT_EQ(jvm::escapeJasminString("hello"), "hello");
}

TEST(EscapeJasminString, EscapesQuoteAndBackslash) {
    EXPECT_EQ(jvm::escapeJasminString("say \"hi\""), "say \\\"hi\\\"");
    EXPECT_EQ(jvm::escapeJasminString("a\\b"), "a\\\\b");
}

TEST(EscapeJasminString, EscapesNewlineTabCarriageReturn) {
    EXPECT_EQ(jvm::escapeJasminString("a\nb\tc\rd"), "a\\nb\\tc\\rd");
}

TEST(EscapeJasminString, EscapesOtherControlBytesAsOctal) {
    EXPECT_EQ(jvm::escapeJasminString(std::string(1, '\x01')), "\\001");
    EXPECT_EQ(jvm::escapeJasminString(std::string(1, '\x1f')), "\\037");
}

TEST(EscapeJasminString, EscapesHighBytesAsOctal) {
    // 0xFF: the source file must carry no raw byte a text-mode read could
    // reinterpret — LoxRuntime.CHARSET (ISO-8859-1) needs char code 255 back.
    EXPECT_EQ(jvm::escapeJasminString(std::string(1, '\xff')), "\\377");
}

// ---------------------------------------------------------------------------
// formatJasminDouble
// ---------------------------------------------------------------------------

TEST(FormatJasminDouble, IntegerValuedGetsDecimalPoint) {
    EXPECT_EQ(jvm::formatJasminDouble(2.0), "2.0");
    EXPECT_EQ(jvm::formatJasminDouble(-6.0), "-6.0");
}

TEST(FormatJasminDouble, AlreadyFractionalIsLeftAlone) {
    // 0.5 is exact in binary, so 17 significant digits print no noise —
    // unlike 5.4, whose 17-digit expansion is genuinely part of the value
    // (%.17g exists for round-trip fidelity, not for pretty-printing; that
    // is stringify's job in the runtime, not this constant encoding).
    EXPECT_EQ(jvm::formatJasminDouble(0.5), "0.5");
}

TEST(FormatJasminDouble, RoundTripsExactly) {
    double values[] = {0.1, 1.0 / 3.0, 1e300, -1e-300, 0.0, -0.0};
    for (double v : values) {
        double parsed = std::stod(jvm::formatJasminDouble(v));
        EXPECT_EQ(std::bit_cast<uint64_t>(parsed), std::bit_cast<uint64_t>(v))
            << "value " << v;
    }
}

// ---------------------------------------------------------------------------
// emitScript
// ---------------------------------------------------------------------------

TEST(EmitScript, AbortsOnUnsupportedOpcode) {
    MemoryManager mm;
    // `if` compiles to JUMP_IF_FALSE — N5's job, not N4's.
    DecodedFunction fn = decodeScript("if (true) { print 1; }", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    try {
        jvm::emitScript(fn, analysis, "LoxMain");
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        EXPECT_EQ(std::string(e.what()),
                  "not implemented in N4: JUMP_IF_FALSE");
    }
}

TEST(EmitScript, AssignLocalFusesSetLocalWithTempPop) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript("{ var a = 1; a = 2; print a; }", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    EXPECT_NE(j.find(".class public LoxMain\n"), std::string::npos);
    // Two live slots at once (callee + `a`): 2 (args, globals) + 2 + 1
    // (scratch).
    EXPECT_NE(j.find(".limit locals 5\n"), std::string::npos);
    // Peak depth is 2 words: the boxed `a = 2` result plus one live
    // temporary never coincide with anything wider in this probe.
    EXPECT_NE(j.find(".limit stack 2\n"), std::string::npos);
    // The SET_LOCAL;POP(TEMP) idiom folds to one astore — no dup anywhere,
    // and no standalone `pop` (the TEMP pop is fused away, the
    // LOCAL-RECLAIM pop at the closing brace needs no instruction at all).
    EXPECT_EQ(countOccurrences(j, "dup"), 0);
    EXPECT_EQ(countOccurrences(j, "\n    pop\n"), 0);
    // `a`'s declaring push (the invisible var) and its later reassignment
    // both store to the same slot.
    EXPECT_EQ(countOccurrences(j, "astore 3\n"), 2);
    EXPECT_EQ(countOccurrences(j, "aload 3\n"), 1);
}

TEST(EmitScript, NestedArithHasNoLocalsAndNoGlobals) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript("print (1 + 2) * (3 - 4) / 5 - -6;", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    EXPECT_NE(j.find(".limit locals 4\n"), std::string::npos);
    EXPECT_NE(j.find("invokestatic lox/LoxOps/add"), std::string::npos);
    EXPECT_NE(j.find("invokestatic lox/LoxOps/subtract"), std::string::npos);
    EXPECT_NE(j.find("invokestatic lox/LoxOps/multiply"), std::string::npos);
    EXPECT_NE(j.find("invokestatic lox/LoxOps/divide"), std::string::npos);
    EXPECT_NE(j.find("invokestatic lox/LoxOps/negate"), std::string::npos);
    EXPECT_NE(j.find("invokestatic lox/LoxOps/print"), std::string::npos);
    // LoxRuntime.init() still runs (every script initializes globals up
    // front), but this probe never defines, reads, or sets one.
    EXPECT_EQ(j.find("invokevirtual lox/LoxGlobals"), std::string::npos);
}

TEST(EmitScript, GlobalsRoundTripThroughDefineSetGet) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript("var x = 1;\nx = 2;\nprint x;\n", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    EXPECT_NE(
        j.find("invokevirtual "
               "lox/LoxGlobals/define(Ljava/lang/String;Ljava/lang/Object;)V"),
        std::string::npos);
    EXPECT_NE(
        j.find("invokevirtual "
               "lox/LoxGlobals/set(Ljava/lang/String;Ljava/lang/Object;)V"),
        std::string::npos);
    EXPECT_NE(
        j.find("invokevirtual "
               "lox/LoxGlobals/get(Ljava/lang/String;)Ljava/lang/Object;"),
        std::string::npos);
    EXPECT_NE(j.find("ldc \"x\""), std::string::npos);
}

// ---------------------------------------------------------------------------
// R1 regression (PR #107 round 1): a SET_LOCAL/SET_GLOBAL peek whose source
// value N2 already folded into a named local (the eager invisible-var
// materialization, abstract_stack.h) must load that local, not assume a JVM
// stack temp that was never pushed. The bug produced a structurally invalid
// method that still returned normally from emitScript, so these tests assert
// the emitted text, not only the absence of a throw.
// ---------------------------------------------------------------------------

TEST(EmitScript, SetLocalPeekOfNamedLocalLoadsInsteadOfDup) {
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("{ var a = 1; var b = (a = 2); print a; print b; }", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    // Slots: 2 (args, globals) + 2 (a, b) + 1 (scratch, unused here) = 5;
    // limit locals is scratchSlot + 1 = 6.
    EXPECT_NE(j.find(".limit locals 6\n"), std::string::npos);
    EXPECT_NE(j.find(".limit stack 2\n"), std::string::npos);
    // No dup anywhere: before[SET_LOCAL(a)].operandDepth() == 0, so the fix
    // takes the load-from-slot branch, never the dup branch.
    EXPECT_EQ(countOccurrences(j, "dup"), 0);
    // The fix in one line: load `b`'s slot (4), the value SET_LOCAL(a) is
    // peeking, then store it into `a`'s slot (3).
    EXPECT_NE(j.find("aload 4\n    astore 3\n"), std::string::npos);
    // `a` gets its declaring store (invisible var) and the R1-fixed store;
    // `b` gets only its declaring store.
    EXPECT_EQ(countOccurrences(j, "astore 3\n"), 2);
    EXPECT_EQ(countOccurrences(j, "astore 4\n"), 1);
}

TEST(EmitScript, SetGlobalPeekOfNamedLocalLoadsInsteadOfScratch) {
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("var g = 0; { var b = g = 9; print b; print g; }", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    // Slots: 2 (args, globals) + 1 (b) + 1 (scratch, unused here) = 4;
    // limit locals is scratchSlot + 1 = 5.
    EXPECT_NE(j.find(".limit locals 5\n"), std::string::npos);
    // The fix: load `b`'s slot (3), then the non-peek LoxGlobals.set call —
    // never the scratch-astore peek path (that astore is what previously
    // underflowed with nothing on the operand stack to consume).
    EXPECT_NE(j.find("aload 3\n"
                     "    aload 1\n"
                     "    swap\n"
                     "    ldc \"g\"\n"
                     "    swap\n"
                     "    invokevirtual "
                     "lox/LoxGlobals/set(Ljava/lang/String;Ljava/lang/"
                     "Object;)V\n"),
              std::string::npos);
    // Slot 4 is this program's scratch slot; the peek path would have
    // written to it. It must stay untouched.
    EXPECT_EQ(countOccurrences(j, "astore 4"), 0);
}

TEST(EmitScript, StringConstantIsEscaped) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript(R"(print "say \"hi\"\n";)", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    EXPECT_NE(j.find(R"(ldc "say \"hi\"\n")"), std::string::npos);
}
