// test_jvm_emit.cpp — JVM straight-line + control-flow emitter (nodes N4/N5).
//
// Checkpoint (notes/backend-implementation-dag.md, nodes N4/N5):
//   tools/loxpp_jvm.sh notes/translation-probes/{01,02,03,04,05,15}_*.lox
// must each print stdout identical to build/loxpp on the same file, and the
// assembled class must pass `java -Xverify:all`. That full assemble-and-run
// comparison needs jasmin/java (tools/check_jvm_probes.sh, run inside the
// dev-managed container); this file covers what a plain C++ unit test can
// check without them: the escaping/formatting helpers, the generated
// Jasmin's structural shape (limits, fusion, labels, goto/ifne,
// abort-on-unsupported).

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
#include <vector>

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

// The mnemonics this emitter writes as a jump (nodes N4/N5/N6/N7). N7 adds
// `ifeq` (the captured-slot raw/cell runtime check — emitCapturedGetLocal,
// emitCapturedStore, ensureCapturedCell); N10 adds `tableswitch`/
// `lookupswitch` (match). Whichever node adds a new jump form must add its
// mnemonic here too, or that jump gets no label-integrity coverage from
// this helper.
const std::vector<std::string> kJumpMnemonics = {"goto ", "ifne ", "ifeq "};

// N6.md (assigned nit, PR #109 R9): a `goto`/`ifne` to a jasmin label the
// emitter never wrote assembles fine as far as ctest can see — only
// tools/check_jvm_probes.sh (jasmin + java, container-only) would ever
// reject it. N5's own R1 was exactly this class of bug, on probe 22.
// Collects every operand of `goto ` and `ifne `, then asserts a "<name>:"
// line exists for each one, so a plain unit test in this file catches the
// same defect at zero runtime cost. Call at the end of every emitScript
// test; every node after N6 that adds a jump inherits the net for free.
//
// PR #110 R6: an earlier version searched for the bare mnemonic anywhere in
// `j`, so a string literal payload containing that text (`ldc "goto
// L_0000"`) matched too, and the label search for that bogus operand then
// failed on a correct program. Anchoring the search to "\n    " — the exact
// indent Builder::emit writes for every real instruction — fixes this,
// because escapeJasminString always renders a raw newline as the two
// characters "\\n", so a real newline followed by four spaces can never
// occur inside a string literal's payload. Prefixing `j` itself with one
// "\n" gives a jump on the very first line the same leading newline to
// anchor against, so no special case is needed there.
void expectEveryJumpTargetIsLabeled(const std::string& j) {
    const std::string text = "\n" + j;
    for (const std::string& mnemonic : kJumpMnemonics) {
        const std::string anchored = "\n    " + mnemonic;
        std::size_t pos = 0;
        while ((pos = text.find(anchored, pos)) != std::string::npos) {
            std::size_t nameStart = pos + anchored.size();
            std::size_t nameEnd = text.find('\n', nameStart);
            ASSERT_NE(nameEnd, std::string::npos)
                << mnemonic << "operand runs off the end of:\n"
                << j;
            // The operand is the first field: a defensive split, not a
            // reaction to any real trailing content this emitter writes
            // today (nodes/N6.md R6, point 2).
            std::string rest = text.substr(nameStart, nameEnd - nameStart);
            std::size_t sep = rest.find_first_of(" \t");
            std::string target =
                (sep == std::string::npos) ? rest : rest.substr(0, sep);
            EXPECT_NE(text.find("\n" + target + ":\n"), std::string::npos)
                << "jump to undefined label \"" << target << "\" in:\n"
                << j;
            pos = nameEnd;
        }
    }
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
// formatDoubleBitsLiteral
//
// PR #107 R6/R7 (round 2): the prior version of this helper formatted a
// decimal/exponent literal for `ldc2_w`. jasmin 2.4 reads that literal shape
// at 32-bit float precision (silently rounding, R6) or rejects some valid
// `%g`-style exponent text outright (R7). std::stod is not the real
// consumer and cannot see either fault (R8) — these tests decode the
// literal the same way the paired `invokestatic .../longBitsToDouble(J)D`
// does: reinterpret the bits, do not re-parse as a double.
// ---------------------------------------------------------------------------

TEST(FormatDoubleBitsLiteral, RoundTripsExactly) {
    double values[] = {2.0,       -6.0,       0.5,  0.1,
                       1.0 / 3.0, 0.0,        -0.0, 1e300,
                       -1e-300,   16777217.0, 1e17, 3.14159265358979};
    for (double v : values) {
        std::string literal = jvm::formatDoubleBitsLiteral(v);
        int64_t bits = std::stoll(literal);
        double roundTripped = std::bit_cast<double>(bits);
        EXPECT_EQ(std::bit_cast<uint64_t>(roundTripped),
                  std::bit_cast<uint64_t>(v))
            << "value " << v << " literal " << literal;
    }
}

TEST(FormatDoubleBitsLiteral, IsABareDecimalIntegerNeverADecimalOrExponent) {
    // The R6/R7 fix's entire point: no '.' and no 'e'/'E', on the exact
    // values that used to break jasmin one way (R6) or the other (R7).
    double values[] = {16777217.0, 1e17, 1e21, -1e-300, 0.1, 3.14159265358979};
    for (double v : values) {
        std::string literal = jvm::formatDoubleBitsLiteral(v);
        EXPECT_EQ(literal.find('.'), std::string::npos) << literal;
        EXPECT_EQ(literal.find('e'), std::string::npos) << literal;
        EXPECT_EQ(literal.find('E'), std::string::npos) << literal;
    }
}

// ---------------------------------------------------------------------------
// emitScript
// ---------------------------------------------------------------------------

TEST(EmitScript, AbortsOnUnsupportedOpcode) {
    MemoryManager mm;
    // An empty map literal compiles to BUILD_MAP — node N9's job, not N8's.
    DecodedFunction fn = decodeScript("var m = {};", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    try {
        jvm::emitScript(fn, analysis, "LoxMain");
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        EXPECT_EQ(std::string(e.what()), "not implemented in N6: BUILD_MAP");
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
    expectEveryJumpTargetIsLabeled(j);
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
    expectEveryJumpTargetIsLabeled(j);
}

TEST(EmitScript, NumberConstantUsesLongBitsRoundTrip) {
    // PR #107 R6/R7: a float-imprecise constant (16777217, the smallest
    // integer a float cannot hold exactly) must not go through a
    // decimal/exponent `ldc2_w` literal.
    MemoryManager mm;
    DecodedFunction fn = decodeScript("print 16777217;", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    std::string expectedBits =
        std::to_string(std::bit_cast<int64_t>(static_cast<double>(16777217)));
    EXPECT_NE(j.find("ldc2_w " + expectedBits +
                     "\n"
                     "    invokestatic java/lang/Double/longBitsToDouble(J)D\n"
                     "    invokestatic "
                     "java/lang/Double/valueOf(D)Ljava/lang/Double;\n"),
              std::string::npos)
        << j;
    expectEveryJumpTargetIsLabeled(j);
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
    expectEveryJumpTargetIsLabeled(j);
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
    expectEveryJumpTargetIsLabeled(j);
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
    expectEveryJumpTargetIsLabeled(j);
}

TEST(EmitScript, StringConstantIsEscaped) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript(R"(print "say \"hi\"\n";)", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    EXPECT_NE(j.find(R"(ldc "say \"hi\"\n")"), std::string::npos);
    expectEveryJumpTargetIsLabeled(j);
}

// ---------------------------------------------------------------------------
// Control flow (node N5): JUMP, JUMP_IF_FALSE, LOOP, labels.
// ---------------------------------------------------------------------------

TEST(EmitScript, IfElseDupsThePeekAndBothPopsAreReal) {
    // 02_if_else: `dup` preserves JUMP_IF_FALSE's peeked condition on the
    // taken edge too, so each side's own, ordinary POP (the fall-through's
    // and the else-target's) discards a real copy — two real `pop`
    // instructions, not one fused away (see the JUMP_IF_FALSE case's own
    // comment for why this pass does not fuse them).
    MemoryManager mm;
    DecodedFunction fn = decodeScript("if (true) print 1; else print 2;", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    EXPECT_EQ(countOccurrences(j, "dup"), 1);
    EXPECT_NE(j.find("invokestatic lox/LoxOps/isFalsy"), std::string::npos);
    EXPECT_NE(j.find("ifne L_"), std::string::npos);
    EXPECT_NE(j.find("goto L_"), std::string::npos); // skip the else branch
    EXPECT_EQ(countOccurrences(j, "\n    pop\n"), 2);
    expectEveryJumpTargetIsLabeled(j);
}

TEST(EmitScript, AndOrKeepsTheValue) {
    // 03_and_or: the jump target is PRINT (the merge that uses the
    // short-circuit result), not a POP — the `dup`'d copy is what survives
    // to be printed; only the fall-through side's own POP is real.
    MemoryManager mm;
    DecodedFunction fn = decodeScript("print 1 and 2;", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    EXPECT_NE(j.find("dup"), std::string::npos);
    EXPECT_EQ(countOccurrences(j, "\n    pop\n"), 1);
    expectEveryJumpTargetIsLabeled(j);
}

// ---------------------------------------------------------------------------
// PR #109 round 1 regressions.
// ---------------------------------------------------------------------------

TEST(EmitScript, AndOrAssignmentStatementKeepsTheMergeLabelReal) {
    // R1 (blocking): probe 22. The short-circuit merge's own POP can also be
    // a CFG block leader when the right side is an assignment — every edge
    // into it needs a real jasmin label there. `fusablePop` must not fuse
    // that POP away, or the label disappears with it and jasmin fails to
    // assemble ("Label ... has not been added to the code").
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("var x = true; var y = 0; x and (y = 1); print y;", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    std::size_t ifnePos = j.find("ifne L_");
    ASSERT_NE(ifnePos, std::string::npos) << j;
    std::size_t nameStart = ifnePos + 5; // skip "ifne "
    std::string target =
        j.substr(nameStart, j.find('\n', nameStart) - nameStart);
    // The label this `ifne` targets must exist as a real "name:" line, and
    // the merge must still have its own, real `pop` — proof the fuse did
    // not eat either one.
    EXPECT_NE(j.find(target + ":\n"), std::string::npos) << j;
    EXPECT_NE(j.find("\n    pop\n"), std::string::npos) << j;
    expectEveryJumpTargetIsLabeled(j);
}

TEST(EmitScript, JumpIfFalseOnAMaterializedConditionLoadsInsteadOfDup) {
    // R2 (blocking): probe 23. When a local's initializer is a short-circuit
    // expression, N2's eager invisible-var materialization (P2/P3) moves the
    // condition off the JVM operand stack before JUMP_IF_FALSE runs —
    // before[i].operandDepth() == 0. `dup` on that empty stack is a
    // VerifyError ("Unable to pop operand off an empty stack"), not a wrong
    // slot; the fix loads a fresh copy from lastInvisibleVarSlot instead.
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("{ var c = true; var b = c and 2; print b; }", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    EXPECT_EQ(countOccurrences(j, "dup"), 0) << j;
    EXPECT_NE(j.find("invokestatic lox/LoxOps/isFalsy"), std::string::npos)
        << j;
    expectEveryJumpTargetIsLabeled(j);
}

TEST(EmitScript, WhileLoopEmitsBackEdgeAndLabel) {
    // 04_while: LOOP lowers to `goto`, at a label N1 placed at the
    // condition. The back edge's own target must be a real, defined label
    // in this same method — not merely present as `goto` text.
    MemoryManager mm;
    DecodedFunction fn = decodeScript(
        "{ var i = 0; while (i < 3) { print i; i = i + 1; } }", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    std::size_t gotoPos = j.find("goto L_");
    ASSERT_NE(gotoPos, std::string::npos);
    // R7 fix (PR #109 nit): `cfg.cpp`'s `"L_%04d"` is a minimum width, not a
    // fixed one — a chunk past 9999 bytes prints a 5th digit. Read to the end
    // of the line instead of a fixed 6 characters, so this test stays
    // correct at any chunk size.
    std::size_t nameStart = gotoPos + 5; // skip "goto "
    std::string target =
        j.substr(nameStart, j.find('\n', nameStart) - nameStart);
    EXPECT_NE(j.find(target + ":"), std::string::npos) << j;
    expectEveryJumpTargetIsLabeled(j);
}

TEST(EmitScript, JumpTargetHelperIgnoresJumpMnemonicInsideStringLiteral) {
    // PR #110 R6: the string payload holds "goto " as ordinary text, not a
    // jasmin instruction. A real back edge is also present, so the helper
    // must find and confirm that one while ignoring the literal's payload.
    MemoryManager mm;
    DecodedFunction fn = decodeScript(
        "var i = 0; while (i < 1) { print \"goto L_0000\"; i = i + 1; }", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    ASSERT_NE(j.find("ldc \"goto L_0000\""), std::string::npos) << j;
    EXPECT_NE(j.find("goto L_"), std::string::npos) << j;
    expectEveryJumpTargetIsLabeled(j);
}

TEST(EmitScript, ForLoopHasTwoBackEdges) {
    // 05_for: the DAG's own verified fact (SESSION-LOG.md) — 2 back edges
    // (LOOP at 25 and 31) plus 1 forward skip (JUMP at 13).
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("for (var i = 0; i < 3; i = i + 1) print i;", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    EXPECT_EQ(countOccurrences(j, "goto L_"), 3);
    expectEveryJumpTargetIsLabeled(j);
}

TEST(EmitScript, IfWithoutElseStillEmitsTheUnconditionalSkip) {
    // No else branch: Compiler::ifStatement (compiler.cpp) emits the
    // unconditional "skip the else" JUMP unconditionally too, even with
    // nothing to skip — this pass lowers whatever the compiler emitted, not
    // a structural guess of when a JUMP "should" be there.
    MemoryManager mm;
    DecodedFunction fn = decodeScript("if (true) print 1;\nprint 2;", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    EXPECT_NE(j.find("ifne L_"), std::string::npos);
    EXPECT_EQ(countOccurrences(j, "goto L_"), 1);
    expectEveryJumpTargetIsLabeled(j);
}

// ---------------------------------------------------------------------------
// R9 regression (N5.md, "inherited from N4"): a SET_LOCAL/SET_GLOBAL whose
// merge-exact operandDepth() is 0 must resolve its source slot from
// lastInvisibleVarSlot (this pass's own forward walk), never from
// `before[i].localCount` — a mere upper bound once a CFG merge exists
// upstream. These assert the store lands in the *same* slot on every path
// and that the depth-consistency safety net (R1) does not trip, which is
// exactly the check a wrong-slot store would fail.
// ---------------------------------------------------------------------------

TEST(EmitScript, IfElseAssignsSameSlotOnBothBranches) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript(
        "{ var a = 1; if (a == 1) { a = 2; } else { a = 3; } print a; }", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    // `a` is the only local (slot 1 in Lox terms) -> JVM slot 3. Both
    // branches must store to it, not to two different slots.
    EXPECT_EQ(countOccurrences(j, "astore 3\n"), 3); // decl + both branches
    expectEveryJumpTargetIsLabeled(j);
}

TEST(EmitScript, LoopBodyAssignsSameSlotEveryIteration) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript(
        "{ var a = 1; var i = 0; while (i < 3) { a = a + 1; i = i + 1; } }",
        mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    // `a` (JVM slot 3) is reassigned once per loop body pass; the same slot
    // must appear each time this pass walks the (single, static) body.
    EXPECT_NE(j.find("astore 3\n"), std::string::npos);
    expectEveryJumpTargetIsLabeled(j);
}

TEST(EmitScript, PeekOfNamedLocalAfterAMergeStillLoadsTheRightSlot) {
    // The R1/R9 idiom (var b = (a = 2)) placed after an if-without-else
    // merge: proves lastInvisibleVarSlot survives a preceding CFG join,
    // where `before[i].localCount` would only be an upper bound.
    MemoryManager mm;
    DecodedFunction fn = decodeScript("{\n"
                                      "  var a = 1;\n"
                                      "  if (a == 1) { print 9; }\n"
                                      "  var b = (a = 2);\n"
                                      "  print a;\n"
                                      "  print b;\n"
                                      "}\n",
                                      mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    // `a` is JVM slot 3, `b` is JVM slot 4 (2 args/globals + 2 locals).
    // Reaching this line at all (no R1 depth-consistency throw) already
    // proves lastInvisibleVarSlot tracked correctly across the merge; the
    // reload-from-slot shape confirms it named the right one.
    EXPECT_NE(j.find("aload 4\n    astore 3\n"), std::string::npos) << j;
    expectEveryJumpTargetIsLabeled(j);
}

// ---------------------------------------------------------------------------
// Functions and calls (node N6): CALL, zero-upvalue CLOSURE, RETURN's dual
// role, and emitProgram's multi-class output.
// ---------------------------------------------------------------------------

TEST(EmitScript, CallWithZeroArgsBuildsEmptyArray) {
    MemoryManager mm;
    // `foo` is never declared — GET_GLOBAL throws at run time (late
    // binding), but this pass only lowers text, it never executes the
    // program, so an undefined callee is a fine probe for CALL's own shape.
    DecodedFunction fn = decodeScript("foo();", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    // argCount == 0: the callee is already the sole, topmost stack value, so
    // the empty array builds directly on top of it — no spill, no scratch
    // slot at all.
    EXPECT_NE(j.find("iconst_0\n"
                     "    anewarray java/lang/Object\n"
                     "    invokestatic "
                     "lox/LoxOps/call(Ljava/lang/Object;[Ljava/lang/Object;)"
                     "Ljava/lang/Object;\n"),
              std::string::npos)
        << j;
    // No locals, no call-arg scratch reserved: 2 (args, globals) + 1
    // (forced minimum local) + 1 (SET_GLOBAL-peek scratch) = 4.
    EXPECT_NE(j.find(".limit locals 4\n"), std::string::npos) << j;
    expectEveryJumpTargetIsLabeled(j);
}

TEST(EmitScript, CallWithArgsSpillsToScratchSlotsAndBuildsArray) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript("foo(1, 2, 3);", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    // No locals: scratchSlot = 3 (2 args/globals + 1 forced minimum). The
    // widest CALL takes 3 arguments, so one callee slot (4) plus one slot
    // per argument (5, 6, 7) come on top of the usual +1 scratch: 3+1+4 = 8.
    EXPECT_NE(j.find(".limit locals 8\n"), std::string::npos) << j;

    // P7: the values are already on the stack in push order [callee, arg1,
    // arg2, arg3], topmost first — so the topmost (arg3) is spilled first,
    // then arg2, then arg1, then the callee underneath them all.
    EXPECT_NE(j.find("astore 7\n"
                     "    astore 6\n"
                     "    astore 5\n"
                     "    astore 4\n"),
              std::string::npos)
        << j;
    EXPECT_NE(j.find("aload 4\n"
                     "    iconst_3\n"
                     "    anewarray java/lang/Object\n"),
              std::string::npos)
        << j;
    EXPECT_EQ(countOccurrences(j, "aastore"), 3);
    EXPECT_EQ(countOccurrences(j, "dup"), 3);
    EXPECT_NE(j.find("invokestatic "
                     "lox/LoxOps/call(Ljava/lang/Object;[Ljava/lang/Object;)"
                     "Ljava/lang/Object;"),
              std::string::npos)
        << j;
    expectEveryJumpTargetIsLabeled(j);
}

// BUILD_LIST/GET_INDEX/SET_INDEX (node N7 pulls these three opcodes forward
// from N9's own scope — see emitBuildList's own note and jvm_emitter.h).

TEST(EmitScript, BuildListOfZeroElementsBuildsDirectly) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript("var e = [];", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    EXPECT_NE(j.find("iconst_0\n"
                     "    anewarray java/lang/Object\n"
                     "    invokestatic "
                     "lox/LoxOps/buildList([Ljava/lang/Object;)Llox/"
                     "LoxList;\n"),
              std::string::npos)
        << j;
    expectEveryJumpTargetIsLabeled(j);
}

TEST(EmitScript, BuildListSpillsToScratchSlotsAndBuildsArray) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript("var xs = [1, 2, 3];", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    // P7, same shape as CALL's own spill (emitCall): the elements are
    // already on the stack in push order [e0, e1, e2], topmost (e2) first,
    // so the topmost is spilled first.
    EXPECT_NE(j.find("astore 7\n"
                     "    astore 6\n"
                     "    astore 5\n"),
              std::string::npos)
        << j;
    EXPECT_NE(j.find("iconst_3\n"
                     "    anewarray java/lang/Object\n"
                     "    dup\n"
                     "    iconst_0\n"
                     "    aload 5\n"
                     "    aastore\n"),
              std::string::npos)
        << j;
    EXPECT_NE(j.find("invokestatic "
                     "lox/LoxOps/buildList([Ljava/lang/Object;)Llox/"
                     "LoxList;\n"),
              std::string::npos)
        << j;
    EXPECT_EQ(countOccurrences(j, "aastore"), 3);
    expectEveryJumpTargetIsLabeled(j);
}

TEST(EmitScript, GetIndexAndSetIndexAreOneInvokestaticEach) {
    // vm.cpp's own operand order already matches LoxOps's parameter order
    // (emitSimpleOp's own note), so neither needs a shuffle — unlike
    // SET_LOCAL/SET_GLOBAL/SET_UPVALUE, this is not a P2 peek.
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("var xs = [1]; print xs[0]; xs[0] = 9;", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    EXPECT_NE(j.find("invokestatic "
                     "lox/LoxOps/getIndex(Ljava/lang/Object;Ljava/lang/"
                     "Object;)Ljava/lang/Object;\n"),
              std::string::npos)
        << j;
    EXPECT_NE(j.find("invokestatic "
                     "lox/LoxOps/setIndex(Ljava/lang/Object;Ljava/lang/"
                     "Object;Ljava/lang/Object;)Ljava/lang/Object;\n"
                     "    pop\n"),
              std::string::npos)
        << j;
    expectEveryJumpTargetIsLabeled(j);
}

// BUILD_MAP/SLICE/IN/GET_ITER/ITER_HAS_NEXT/ITER_NEXT/IS_SEQ (node N9:
// notes/backend-implementation-dag.md, "aggregates, indexing, iterators").

TEST(EmitScript, BuildMapOfZeroPairsBuildsDirectly) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript("var m = {};", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    EXPECT_NE(j.find("iconst_0\n"
                     "    anewarray java/lang/Object\n"
                     "    invokestatic "
                     "lox/LoxOps/buildMap([Ljava/lang/Object;)Llox/"
                     "LoxMap;\n"),
              std::string::npos)
        << j;
    expectEveryJumpTargetIsLabeled(j);
}

TEST(EmitScript, BuildMapSpillsToScratchSlotsAndBuildsArray) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript("var m = {\"a\": 1, \"b\": 2};", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    // P7, same shape as BUILD_LIST's own spill, doubled: 2 pairs push 4
    // cells [key0, val0, key1, val1], topmost (val1) first, so the topmost
    // is spilled first.
    EXPECT_NE(j.find("astore 8\n"
                     "    astore 7\n"
                     "    astore 6\n"
                     "    astore 5\n"),
              std::string::npos)
        << j;
    EXPECT_NE(j.find("iconst_4\n"
                     "    anewarray java/lang/Object\n"
                     "    dup\n"
                     "    iconst_0\n"
                     "    aload 5\n"
                     "    aastore\n"),
              std::string::npos)
        << j;
    EXPECT_NE(j.find("invokestatic "
                     "lox/LoxOps/buildMap([Ljava/lang/Object;)Llox/"
                     "LoxMap;\n"),
              std::string::npos)
        << j;
    EXPECT_EQ(countOccurrences(j, "aastore"), 4);
    expectEveryJumpTargetIsLabeled(j);
}

TEST(EmitScript, SliceAndInAreOneInvokestaticEach) {
    // vm.cpp's own operand order already matches each helper's own parameter
    // order (LoxOps.slice's and LoxOps.in's own doc comments), so neither
    // needs a shuffle.
    MemoryManager mm;
    DecodedFunction fn = decodeScript(
        "var s = \"hello\"; print s[1:3]; print 1 in [1, 2, 3];", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    EXPECT_NE(j.find("invokestatic "
                     "lox/LoxOps/slice(Ljava/lang/Object;Ljava/lang/Object;"
                     "Ljava/lang/Object;)Ljava/lang/Object;\n"),
              std::string::npos)
        << j;
    EXPECT_NE(j.find("invokestatic lox/LoxOps/in(Ljava/lang/Object;Ljava/lang/"
                     "Object;)Z\n"
                     "    invokestatic java/lang/Boolean/valueOf(Z)Ljava/lang/"
                     "Boolean;\n"),
              std::string::npos)
        << j;
    expectEveryJumpTargetIsLabeled(j);
}

TEST(EmitScript, GetIterReloadsAndRestoresItsOwnDeclaringSlot) {
    // GET_ITER carries no operand byte and replaces its own operand in
    // place (vm.cpp: `stackTop[-1] = ...`) — N2/N3 attribute the invisible-
    // var store for that position to the iterable expression's OWN
    // declaring push (here BUILD_LIST), one instruction earlier, so the JVM
    // operand stack is already empty by the time GET_ITER runs. A plain
    // one-`invokestatic` lowering (no reload, no store-back) calls
    // LoxOps.getIter on an empty stack: jasmin's own net-word bookkeeping
    // cannot catch this, because GET_ITER's declared effect is a true net
    // zero (one popped, one pushed) — only `java -Xverify:all` does
    // (`VerifyError: Unable to pop operand off an empty stack`,
    // check_jvm_probes.sh on 11_for_in.lox). This test fails without the
    // `aload <slot>` / `astore <slot>` wrap: reverting emitGetIter to a bare
    // `invokestatic getIter` call, with no surrounding load/store, makes it
    // fail (verified locally against this PR's own diff).
    MemoryManager mm;
    DecodedFunction fn = decodeScript("for (var x in [1, 2, 3]) print x;", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    // No locals of its own before the loop: scratchSlot = 3 (2 fixed +
    // forced minimum 1), so argScratchBase = 5, and BUILD_LIST's 3-element
    // spill occupies 5-7 (CallWithArgsSpillsToScratchSlotsAndBuildsArray's
    // own numbering, one node earlier). The iterable's declaring push (and
    // so GET_ITER's own reload/store slot) is the next one after that:
    // JVM local 3 (Lox slot 1 — Lox slot 0 is the script's own callee).
    EXPECT_NE(j.find("invokestatic "
                     "lox/LoxOps/buildList([Ljava/lang/Object;)Llox/"
                     "LoxList;\n"
                     "    astore 3\n"
                     "    aload 3\n"
                     "    invokestatic "
                     "lox/LoxOps/getIter(Ljava/lang/Object;)Llox/"
                     "LoxIterator;\n"
                     "    astore 3\n"),
              std::string::npos)
        << j;
    // P8: the iterator lives in an ordinary chunk local; ITER_HAS_NEXT/
    // ITER_NEXT each consume the copy a preceding GET_LOCAL (aload 3)
    // loaded, never the slot itself.
    EXPECT_NE(j.find("aload 3\n"
                     "    invokestatic lox/LoxOps/iterHasNext(Ljava/lang/"
                     "Object;)Z\n"
                     "    invokestatic java/lang/Boolean/valueOf(Z)Ljava/lang/"
                     "Boolean;\n"),
              std::string::npos)
        << j;
    EXPECT_NE(j.find("aload 3\n"
                     "    invokestatic lox/LoxOps/iterNext(Ljava/lang/Object;)"
                     "Ljava/lang/Object;\n"),
              std::string::npos)
        << j;
    expectEveryJumpTargetIsLabeled(j);
}

TEST(EmitScript, GetIterAfterAMergeStillLoadsTheRightSlot) {
    // R1 fix (PR #112 round 1): emitGetIter used to name its slot with
    // `before[i].height - 1`, the same upper-bound-at-a-merge trap
    // PeekOfNamedLocalAfterAMergeStillLoadsTheRightSlot already proves for
    // SET_LOCAL. This test puts the for-in loop's own iterable push after an
    // if-without-else merge, the same shape that test uses, so `emitGetIter`
    // must read `lastInvisibleVarSlot`, not `before[i].height`, or the
    // R1 guard below throws instead of a wrong slot silently compiling.
    //
    // A genuinely DIVERGENT merge (two edges disagreeing on live local
    // count) needs a consumed match expression as the iterable, which does
    // not emit yet (node N10's gap, recorded in this PR). This test cannot
    // force that divergence on this branch; it proves the fix does not
    // regress the ordinary, non-divergent merge instead — the same honest
    // limit PeekOfNamedLocalAfterAMergeStillLoadsTheRightSlot already
    // accepts for SET_LOCAL.
    MemoryManager mm;
    DecodedFunction fn = decodeScript("{\n"
                                      "  var a = 1;\n"
                                      "  if (a == 1) { print 9; }\n"
                                      "  for (var x in [1, 2, 3]) print x;\n"
                                      "}\n",
                                      mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    // `a` is JVM slot 3 (2 fixed + Lox slot 0's implicit reserved local).
    // Reaching this line at all already proves the R1 guard did not throw.
    // The GET_ITER site's own reload/store slot is JVM slot 4 (Lox slot 2:
    // implicit reserved, then `a`, then the for-in's own "(iter)" local).
    EXPECT_NE(j.find("invokestatic "
                     "lox/LoxOps/buildList([Ljava/lang/Object;)Llox/"
                     "LoxList;\n"
                     "    astore 4\n"
                     "    aload 4\n"
                     "    invokestatic "
                     "lox/LoxOps/getIter(Ljava/lang/Object;)Llox/"
                     "LoxIterator;\n"
                     "    astore 4\n"),
              std::string::npos)
        << j;
    expectEveryJumpTargetIsLabeled(j);
}

TEST(EmitScript, IsSeqEmitsOneInvokestatic) {
    // IS_SEQ is a match sequence-pattern's own type check (compiler.cpp).
    // An unguarded catch-all arm keeps this snippet inside N0-N9's opcode
    // set: no JUMP_TABLE/GET_TAG (those need an enum arm, previewEnumArms
    // rejects a sequence pattern on sight) and no MATCH_ERROR (only emitted
    // when no arm is an unguarded catch-all).
    //
    // The match is a bare statement, its result discarded, not assigned to a
    // variable: assigning it (`var n = match ...;`, also `13_enum_match.lox`'s
    // own shape) hits a separate, pre-existing gap this node does not own —
    // compileMatchBody exposes its result with one native POP that reclaims
    // only the synthetic "subject" local, trusting the native VM's fused
    // local/operand-stack model to leave the already-stored "result" local
    // as the new top of stack (compiler.cpp: "Pop the subject; result_value
    // becomes the match expression result"). The JVM backend has no such
    // fused model, and nothing re-loads the result local afterward, so
    // emission fails with an operand-stack underflow on WHATEVER instruction
    // tries to consume it next — for any match expression, with or without
    // enum tags or sequence patterns. Verified with a minimal repro outside
    // this node's own opcodes: `var n = match 1 { case _ => 5 };` fails
    // identically. That is P8 (match/enum dispatch), N10's own scope, not
    // P7's; recorded for N10 in notes/backend-implementation-dag.md so it is
    // not rediscovered by collision. A match used as a bare, fully-discarded
    // statement does not reach that gap, which is all this opcode-shape test
    // needs.
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("match [1, 2] { case [a, b] => a + b case _ => 0 };", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    EXPECT_NE(j.find("invokestatic lox/LoxOps/isSeq(Ljava/lang/Object;)Z\n"
                     "    invokestatic java/lang/Boolean/valueOf(Z)Ljava/lang/"
                     "Boolean;\n"),
              std::string::npos)
        << j;
    expectEveryJumpTargetIsLabeled(j);
}

TEST(EmitProgram, ZeroUpvalueClosureConstructsGeneratedClass) {
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("fun add(a, b) { return a + b; } print add(1, 2);", mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::vector<jvm::EmittedClass> classes =
        jvm::emitProgram(fn, tree, "LoxMain");

    ASSERT_EQ(classes.size(), 2u);
    EXPECT_EQ(classes[0].className, "LoxMain");
    EXPECT_EQ(classes[1].className, "LoxFn$0");

    const std::string& main = classes[0].source;
    // Zero upvalues (node N6; a captured one is N7's wiring): new + an empty
    // Object[][] + the one constructor every generated class shares.
    EXPECT_NE(main.find("new LoxFn$0\n"
                        "    dup\n"
                        "    iconst_0\n"
                        "    anewarray [Ljava/lang/Object;\n"
                        "    invokespecial "
                        "LoxFn$0/<init>([[Ljava/lang/Object;)V\n"),
              std::string::npos)
        << main;
    expectEveryJumpTargetIsLabeled(main);

    const std::string& fn0 = classes[1].source;
    EXPECT_NE(fn0.find(".class public LoxFn$0\n"), std::string::npos) << fn0;
    EXPECT_NE(fn0.find(".super lox/LoxClosure\n\n"), std::string::npos) << fn0;
    EXPECT_NE(fn0.find(".method public <init>([[Ljava/lang/Object;)V\n"),
              std::string::npos)
        << fn0;
    // <init>'s own literals: this function's compile-time name and arity.
    EXPECT_NE(fn0.find("ldc \"add\"\n"), std::string::npos) << fn0;
    EXPECT_NE(fn0.find("invokespecial "
                       "lox/LoxClosure/<init>(Ljava/lang/String;I[[Ljava/"
                       "lang/Object;)V\n"),
              std::string::npos)
        << fn0;
    EXPECT_NE(fn0.find(".method protected invoke(Ljava/lang/Object;[Ljava/"
                       "lang/Object;)Ljava/lang/Object;\n"),
              std::string::npos)
        << fn0;
    // Argument prologue (P5): self (JVM slot 1) copied into slot 4 (`a`'s
    // Lox-frame-slot-0 mirror, baseSlot=4 for a function chunk), then
    // args[0]/args[1] unpacked into slots 5/6 (`a`, `b`).
    EXPECT_NE(fn0.find("aload 1\n    astore 4\n"), std::string::npos) << fn0;
    EXPECT_NE(fn0.find("aload 2\n"
                       "    iconst_0\n"
                       "    aaload\n"
                       "    astore 5\n"),
              std::string::npos)
        << fn0;
    EXPECT_NE(fn0.find("aload 2\n"
                       "    iconst_1\n"
                       "    aaload\n"
                       "    astore 6\n"),
              std::string::npos)
        << fn0;
    // RETURN's function role: areturn, not the script's void `return`.
    EXPECT_NE(fn0.find("areturn\n"), std::string::npos) << fn0;
    expectEveryJumpTargetIsLabeled(fn0);
}

TEST(EmitProgram, SiblingFunctionsGetSequentialClassNames) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript("fun a() { return 1; }\n"
                                      "fun b() { return 2; }\n"
                                      "print a();\n"
                                      "print b();\n",
                                      mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::vector<jvm::EmittedClass> classes =
        jvm::emitProgram(fn, tree, "LoxMain");

    // Deterministic naming (brief.md section 9): one pre-order counter over
    // the whole tree, not per parent — `a` and `b` are siblings, so they
    // draw 0 and 1 in declaration order.
    ASSERT_EQ(classes.size(), 3u);
    EXPECT_EQ(classes[0].className, "LoxMain");
    EXPECT_EQ(classes[1].className, "LoxFn$0");
    EXPECT_EQ(classes[2].className, "LoxFn$1");
    expectEveryJumpTargetIsLabeled(classes[0].source);
    expectEveryJumpTargetIsLabeled(classes[1].source);
    expectEveryJumpTargetIsLabeled(classes[2].source);
}

// Node N7: closures and upvalues. The real, end-to-end proof that a captured
// local behaves correctly (V1_fresh_cell, V2_shared, V3_loopvar,
// 06_shared_upvalue) is tools/check_jvm_probes.sh, run inside the
// dev-managed container — a plain unit test cannot assemble+run jasmin/java.
// What follows checks the structural shape this pass promises: the
// idempotent seed at a CLOSURE that captures a local, GET/SET_UPVALUE's own
// lowering, and the isLocal=false pass-through for a grandparent's upvalue.

TEST(EmitProgram, SingleUpvalueClosureSeedsAndWiresTheCell) {
    // outer's own slot 1 is `x`. jvmSlotForLocal(1) with baseSlot=4 is 5.
    MemoryManager mm;
    DecodedFunction fn = decodeScript("fun outer() {\n"
                                      "  var x = 0;\n"
                                      "  fun get() { return x; }\n"
                                      "  return get;\n"
                                      "}\n",
                                      mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::vector<jvm::EmittedClass> classes =
        jvm::emitProgram(fn, tree, "LoxMain");

    ASSERT_EQ(classes.size(), 3u);
    EXPECT_EQ(classes[0].className, "LoxMain");
    expectEveryJumpTargetIsLabeled(classes[0].source);
    const std::string& outer = classes[1].source;
    // The idempotent seed (ensureCapturedCell): check, seed only if not
    // already a cell, then read the (now guaranteed) cell for wiring.
    EXPECT_NE(outer.find("aload 5\n"
                         "    instanceof [Ljava/lang/Object;\n"
                         "    ifne Jcok3_0\n"
                         "    iconst_1\n"
                         "    anewarray java/lang/Object\n"
                         "    dup\n"
                         "    iconst_0\n"
                         "    aload 5\n"
                         "    aastore\n"
                         "    astore 5\n"
                         "Jcok3_0:\n"),
              std::string::npos)
        << outer;
    EXPECT_NE(outer.find("new LoxFn$1\n"
                         "    dup\n"
                         "    iconst_1\n"
                         "    anewarray [Ljava/lang/Object;\n"
                         "    dup\n"
                         "    iconst_0\n"
                         "    aload 5\n"
                         "    checkcast [Ljava/lang/Object;\n"
                         "    aastore\n"
                         "    invokespecial LoxFn$1/<init>([[Ljava/lang/"
                         "Object;)V\n"),
              std::string::npos)
        << outer;
    expectEveryJumpTargetIsLabeled(outer);

    const std::string& get = classes[2].source;
    // GET_UPVALUE 0: upvals[0][0].
    EXPECT_NE(get.find("aload 0\n"
                       "    getfield lox/LoxClosure/upvalues [[Ljava/lang/"
                       "Object;\n"
                       "    iconst_0\n"
                       "    aaload\n"
                       "    iconst_0\n"
                       "    aaload\n"
                       "    areturn\n"),
              std::string::npos)
        << get;
    expectEveryJumpTargetIsLabeled(get);
}

TEST(EmitProgram, TwoClosuresShareOneCaptureCell) {
    // 06_shared_upvalue.lox: get and set both capture x. Each CLOSURE gets
    // its OWN idempotent check (distinct labels, tied to its own offset —
    // capLabel), because ensureCapturedCell cannot assume the other one ran
    // first on every path (nodes/N7.md hazard: two closures sharing a cell
    // is not always sequential — an if/else can capture the same outer on
    // mutually exclusive arms). Here both run on the SAME straight-line
    // path, so the second one's check is a real no-op at runtime, but the
    // JVM bytecode still carries both checks.
    MemoryManager mm;
    DecodedFunction fn = decodeScript("fun outer() {\n"
                                      "  var x = 0;\n"
                                      "  fun get() { return x; }\n"
                                      "  fun set(v) { x = v; }\n"
                                      "  return get;\n"
                                      "}\n",
                                      mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::vector<jvm::EmittedClass> classes =
        jvm::emitProgram(fn, tree, "LoxMain");

    ASSERT_EQ(classes.size(), 4u);
    EXPECT_EQ(classes[0].className, "LoxMain");
    expectEveryJumpTargetIsLabeled(classes[0].source);
    const std::string& outer = classes[1].source;
    EXPECT_NE(outer.find("Jcok3_0:"), std::string::npos) << outer;
    EXPECT_NE(outer.find("Jcok8_0:"), std::string::npos) << outer;
    // Both CLOSUREs read the SAME slot (5) for their cell — one shared cell.
    EXPECT_EQ(countOccurrences(outer, "aload 5\n"
                                      "    checkcast [Ljava/lang/Object;\n"
                                      "    aastore\n"),
              2);
    expectEveryJumpTargetIsLabeled(outer);

    const std::string& get = classes[2].source;
    expectEveryJumpTargetIsLabeled(get);

    // set(v): SET_UPVALUE 0 writes upvals[0][0], fused with its own trailing
    // POP (the assignment is a bare statement) — no leftover `aload` of the
    // spilled value.
    const std::string& set = classes[3].source;
    EXPECT_NE(set.find("getfield lox/LoxClosure/upvalues [[Ljava/lang/"
                       "Object;\n"
                       "    iconst_0\n"
                       "    aaload\n"
                       "    iconst_0\n"),
              std::string::npos)
        << set;
    EXPECT_NE(set.find("aastore\n"
                       "    aconst_null\n"
                       "    areturn\n"),
              std::string::npos)
        << set;
    expectEveryJumpTargetIsLabeled(set);
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
    std::vector<jvm::EmittedClass> classes =
        jvm::emitProgram(fn, tree, "LoxMain");

    ASSERT_EQ(classes.size(), 4u);
    const std::string& bFn = classes[2].source;
    EXPECT_EQ(bFn.find("instanceof"), std::string::npos) << bFn;
    EXPECT_NE(bFn.find("dup\n"
                       "    iconst_0\n"
                       "    aload 0\n"
                       "    getfield lox/LoxClosure/upvalues [[Ljava/lang/"
                       "Object;\n"
                       "    iconst_0\n"
                       "    aaload\n"
                       "    aastore\n"),
              std::string::npos)
        << bFn;
    expectEveryJumpTargetIsLabeled(classes[0].source);
    expectEveryJumpTargetIsLabeled(classes[1].source);
    expectEveryJumpTargetIsLabeled(bFn);
    expectEveryJumpTargetIsLabeled(classes[3].source);
}

// Node N7's own harness invariant (nodes/N8.md, "inherited from N7"): a local
// `fun` that captures itself made the JVM verifier reject the class
// ("Accessing value from uninitialized register") until N7's fix
// (seedSelfCaptureCell) seeded the cell before anything read it. This node
// extends the very same emitClosure path — `super` is captured the same
// way (SuperInvokeZeroArgsUsesOneScratchSlot below) — so it is the node
// most likely to break that fix and least likely to notice, since only
// tools/check_jvm_probes.sh (container-only) would catch a regression.
// Reverting seedSelfCaptureCell's seed locally and rerunning this test
// confirms it FAILS first: without the seed, the very first "astore 5"
// this test looks for does not exist before the closure's own array-build
// read of that same slot (it is instead the aastore-redirect after
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
    std::vector<jvm::EmittedClass> classes =
        jvm::emitProgram(fn, tree, "LoxMain");

    ASSERT_EQ(classes.size(), 3u);
    const std::string& outer = classes[1].source;
    // f's own Lox slot 1 (jvmSlotForLocal(1) with baseSlot=4) is 5: the
    // seed (`astore 5`) must precede every later `aload 5` that reads it as
    // a cell — the array-build loop, and the redirected store of the
    // closure itself into the cell.
    std::size_t seedPos = outer.find("astore 5\n");
    ASSERT_NE(seedPos, std::string::npos) << outer;
    std::size_t firstReadPos = outer.find("aload 5\n");
    ASSERT_NE(firstReadPos, std::string::npos) << outer;
    EXPECT_LT(seedPos, firstReadPos)
        << "the seed must run before the first read of the self-captured "
           "slot, or the JVM verifier rejects an uninitialized register:\n"
        << outer;
    expectEveryJumpTargetIsLabeled(classes[0].source);
    expectEveryJumpTargetIsLabeled(outer);
    expectEveryJumpTargetIsLabeled(classes[2].source);
}

// Node N8: classes, methods, super (P5+P4).

TEST(EmitScript, ClassOpcodeConstructsWithNullSuperclass) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript("class Foo {}", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    std::string j = jvm::emitScript(fn, analysis, "LoxMain");

    // CLASS builds with superclass=null; INHERIT (below), not CLASS, fills
    // it in on a class that has one. `new;dup;...;invokespecial` leaves the
    // un-dup'd reference as the pushed result — the same idiom emitClosure
    // already uses to build a generated LoxFn$<n>.
    EXPECT_NE(j.find("new lox/LoxClass\n"
                     "    dup\n"
                     "    ldc \"Foo\"\n"
                     "    aconst_null\n"
                     "    invokespecial lox/LoxClass/<init>(Ljava/lang/"
                     "String;Llox/LoxClass;)V\n"),
              std::string::npos)
        << j;
    expectEveryJumpTargetIsLabeled(j);
}

TEST(EmitProgram, DefineMethodDupsClassAndChecksCastsBothOperands) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript("class Foo { bar() { return 1; } }\n"
                                      "var f = Foo();\n"
                                      "print f.bar();\n",
                                      mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::vector<jvm::EmittedClass> classes =
        jvm::emitProgram(fn, tree, "LoxMain");

    ASSERT_EQ(classes.size(), 2u);
    const std::string& main = classes[0].source;
    // DEFINE_METHOD 'bar': `[cls,fn] -> [cls]` (P2) — `dup` keeps the class
    // value for the class body's own trailing POP; both operands need an
    // explicit checkcast, since LoxOps.defineMethod takes concrete types
    // (an existing, already-tested signature —
    // runtime/jvm/test/lox/ClassesTest.java calls it directly).
    EXPECT_NE(main.find("astore 3\n"
                        "    dup\n"
                        "    checkcast lox/LoxClass\n"
                        "    ldc \"bar\"\n"
                        "    aload 3\n"
                        "    checkcast lox/LoxClosure\n"
                        "    invokestatic lox/LoxOps/defineMethod(Llox/"
                        "LoxClass;Ljava/lang/String;Llox/LoxClosure;)V\n"),
              std::string::npos)
        << main;
    // INVOKE 'bar' 0: argCount==0 needs no reshuffle at all, same as
    // emitCall's own argCount==0 path — the receiver is already the sole,
    // topmost value.
    EXPECT_NE(main.find("ldc \"bar\"\n"
                        "    iconst_0\n"
                        "    anewarray java/lang/Object\n"
                        "    invokestatic lox/LoxOps/invoke(Ljava/lang/"
                        "Object;Ljava/lang/String;[Ljava/lang/Object;)Ljava/"
                        "lang/Object;\n"),
              std::string::npos)
        << main;
    expectEveryJumpTargetIsLabeled(main);
    expectEveryJumpTargetIsLabeled(classes[1].source);
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
    std::vector<jvm::EmittedClass> classes =
        jvm::emitProgram(fn, tree, "LoxMain");

    ASSERT_EQ(classes.size(), 3u);
    const std::string& init = classes[1].source;
    // SET_PROPERTY 'x': `[obj,v] -> [v]` (P2 shuffle) — `v` spills to the
    // scratch slot while the constant name is pushed between receiver and
    // value; `init`'s own implicit `return this;` (GET_LOCAL 0) reuses the
    // same `aload 4` right afterward.
    EXPECT_NE(init.find("aload 4\n"
                        "    aload 5\n"
                        "    astore 6\n"
                        "    ldc \"x\"\n"
                        "    aload 6\n"
                        "    invokestatic lox/LoxOps/setProperty(Ljava/lang/"
                        "Object;Ljava/lang/String;Ljava/lang/Object;)Ljava/"
                        "lang/Object;\n"
                        "    pop\n"
                        "    aload 4\n"
                        "    areturn\n"),
              std::string::npos)
        << init;
    expectEveryJumpTargetIsLabeled(init);

    const std::string& get = classes[2].source;
    // GET_PROPERTY 'x': the receiver (`this`, slot 0) is already on the
    // stack; only the constant name needs pushing before the call.
    EXPECT_NE(get.find("aload 4\n"
                       "    ldc \"x\"\n"
                       "    invokestatic lox/LoxOps/getProperty(Ljava/lang/"
                       "Object;Ljava/lang/String;)Ljava/lang/Object;\n"
                       "    areturn\n"),
              std::string::npos)
        << get;
    expectEveryJumpTargetIsLabeled(get);
    expectEveryJumpTargetIsLabeled(classes[0].source);
}

TEST(EmitProgram, InvokeWithArgsSpillsToScratchSlotsAndBuildsArray) {
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("class C { add(a, b) { return a + b; } }\n"
                     "print C().add(1, 2);\n",
                     mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::vector<jvm::EmittedClass> classes =
        jvm::emitProgram(fn, tree, "LoxMain");

    ASSERT_EQ(classes.size(), 2u);
    const std::string& main = classes[0].source;
    // Same spill shape as CALL (emitCall): the receiver and both args are
    // already loose on the stack, so they spill to scratch slots (in
    // reverse pop order) before the array builds on top of the reloaded
    // receiver.
    EXPECT_NE(main.find("astore 6\n"
                        "    astore 5\n"
                        "    astore 4\n"
                        "    aload 4\n"
                        "    ldc \"add\"\n"
                        "    iconst_2\n"
                        "    anewarray java/lang/Object\n"
                        "    dup\n"
                        "    iconst_0\n"
                        "    aload 5\n"
                        "    aastore\n"
                        "    dup\n"
                        "    iconst_1\n"
                        "    aload 6\n"
                        "    aastore\n"
                        "    invokestatic lox/LoxOps/invoke(Ljava/lang/"
                        "Object;Ljava/lang/String;[Ljava/lang/Object;)Ljava/"
                        "lang/Object;\n"),
              std::string::npos)
        << main;
    expectEveryJumpTargetIsLabeled(main);
    expectEveryJumpTargetIsLabeled(classes[1].source);
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
    std::vector<jvm::EmittedClass> classes =
        jvm::emitProgram(fn, tree, "LoxMain");

    ASSERT_EQ(classes.size(), 3u);
    const std::string& main = classes[0].source;
    // compiler.cpp's fixed shape (namedVariable(superclass); beginScope();
    // addLocal(super); ...; namedVariable(className); INHERIT) means the
    // superclass value is ALWAYS already the "super" invisible var by the
    // time INHERIT runs, not a live operand-stack temp — so this loads it
    // back (`aload 3`) rather than assuming it still sits beneath the
    // subclass on the physical stack.
    EXPECT_NE(main.find("ldc \"A\"\n"
                        "    invokevirtual lox/LoxGlobals/get(Ljava/lang/"
                        "String;)Ljava/lang/Object;\n"
                        "    astore 3\n"
                        "    aload 1\n"
                        "    ldc \"B\"\n"
                        "    invokevirtual lox/LoxGlobals/get(Ljava/lang/"
                        "String;)Ljava/lang/Object;\n"
                        "    aload 3\n"
                        "    invokestatic lox/LoxOps/inheritInto(Ljava/lang/"
                        "Object;Ljava/lang/Object;)V\n"),
              std::string::npos)
        << main;
    expectEveryJumpTargetIsLabeled(main);
    expectEveryJumpTargetIsLabeled(classes[1].source);
    expectEveryJumpTargetIsLabeled(classes[2].source);
}

TEST(EmitProgram, SuperInvokeZeroArgsUsesSwapAndOneScratchSlot) {
    // notes/translation-probes/10_super.lox verbatim — B's own `greet`.
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("class A { greet() { return 1; } }\n"
                     "class B < A { greet() { return super.greet() + 1; } }\n"
                     "print B().greet();\n",
                     mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::vector<jvm::EmittedClass> classes =
        jvm::emitProgram(fn, tree, "LoxMain");

    ASSERT_EQ(classes.size(), 3u);
    const std::string& bGreet = classes[2].source;
    // `this` (from a preceding GET_LOCAL 0) is pushed before `super` (from
    // GET_UPVALUE 0), so a bare `swap` reorders them with no extra slot;
    // `this` then spills to the scratch slot while the name and the empty
    // args array build on top of the reloaded superclass.
    EXPECT_NE(bGreet.find("aaload\n"
                          "    swap\n"
                          "    astore 5\n"
                          "    ldc \"greet\"\n"
                          "    aload 5\n"
                          "    iconst_0\n"
                          "    anewarray java/lang/Object\n"
                          "    invokestatic lox/LoxOps/superInvoke(Ljava/"
                          "lang/Object;Ljava/lang/String;Ljava/lang/Object;"
                          "[Ljava/lang/Object;)Ljava/lang/Object;\n"),
              std::string::npos)
        << bGreet;
    expectEveryJumpTargetIsLabeled(bGreet);
    expectEveryJumpTargetIsLabeled(classes[0].source);
    expectEveryJumpTargetIsLabeled(classes[1].source);
}

TEST(EmitProgram, SuperInvokeWithArgsSpillsThreeDistinctScratchSlots) {
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("class Shape { init(a, b) { this.a = a; this.b = b; } }\n"
                     "class Sub < Shape { init(x) { super.init(x, x); } }\n"
                     "print Sub(3).a;\n",
                     mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::vector<jvm::EmittedClass> classes =
        jvm::emitProgram(fn, tree, "LoxMain");

    ASSERT_EQ(classes.size(), 3u);
    const std::string& subInit = classes[2].source;
    // argCount >= 1 spills the superclass (e.scratchSlot), the args
    // (e.argScratchBase, in reverse pop order), and self
    // (e.calleeScratchSlot) — three DISTINCT, already-existing slots, since
    // no other instruction's own shuffle is live at the same time.
    EXPECT_NE(subInit.find("astore 6\n"
                           "    astore 9\n"
                           "    astore 8\n"
                           "    astore 7\n"
                           "    aload 6\n"
                           "    ldc \"init\"\n"
                           "    aload 7\n"
                           "    iconst_2\n"
                           "    anewarray java/lang/Object\n"
                           "    dup\n"
                           "    iconst_0\n"
                           "    aload 8\n"
                           "    aastore\n"
                           "    dup\n"
                           "    iconst_1\n"
                           "    aload 9\n"
                           "    aastore\n"
                           "    invokestatic lox/LoxOps/superInvoke(Ljava/"
                           "lang/Object;Ljava/lang/String;Ljava/lang/Object;"
                           "[Ljava/lang/Object;)Ljava/lang/Object;\n"),
              std::string::npos)
        << subInit;
    expectEveryJumpTargetIsLabeled(subInit);
    expectEveryJumpTargetIsLabeled(classes[0].source);
    expectEveryJumpTargetIsLabeled(classes[1].source);
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
    std::vector<jvm::EmittedClass> classes =
        jvm::emitProgram(fn, tree, "LoxMain");

    ASSERT_EQ(classes.size(), 3u);
    const std::string& bGreet = classes[2].source;
    // GET_SUPER 'greet': same swap-based reorder as SUPER_INVOKE's own
    // zero-arg path, but the call binds a bound method instead of running
    // it — LoxOps.getSuper's own result is what `f` holds.
    EXPECT_NE(bGreet.find("aaload\n"
                          "    swap\n"
                          "    astore 6\n"
                          "    ldc \"greet\"\n"
                          "    aload 6\n"
                          "    invokestatic lox/LoxOps/getSuper(Ljava/lang/"
                          "Object;Ljava/lang/String;Ljava/lang/Object;)Ljava/"
                          "lang/Object;\n"),
              std::string::npos)
        << bGreet;
    expectEveryJumpTargetIsLabeled(bGreet);
    expectEveryJumpTargetIsLabeled(classes[0].source);
    expectEveryJumpTargetIsLabeled(classes[1].source);
}

TEST(EmitProgram, InstanceofChecksGlobalsByName) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript(
        "class A {}\n"
        "fun f(x) { return match x { case A => 1 case _ => 2 }; }\n"
        "print f(A());\n",
        mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::vector<jvm::EmittedClass> classes =
        jvm::emitProgram(fn, tree, "LoxMain");

    ASSERT_EQ(classes.size(), 2u);
    const std::string& f = classes[1].source;
    // INSTANCEOF 'A': vm.cpp looks the class up BY NAME in globals, not
    // from a constant-pool class reference, so this needs the already-open
    // globals receiver (e.globalsSlot) plus the constant name — never a
    // checkcast, since LoxOps.instanceOf takes Object.
    EXPECT_NE(f.find("aload 3\n"
                     "    ldc \"A\"\n"
                     "    invokestatic lox/LoxOps/instanceOf(Ljava/lang/"
                     "Object;Llox/LoxGlobals;Ljava/lang/String;)Z\n"
                     "    invokestatic java/lang/Boolean/valueOf(Z)Ljava/"
                     "lang/Boolean;\n"),
              std::string::npos)
        << f;
    expectEveryJumpTargetIsLabeled(f);
    expectEveryJumpTargetIsLabeled(classes[0].source);
}

TEST(EmitProgram, MatchErrorCallsLoxOpsMatchError) {
    MemoryManager mm;
    DecodedFunction fn =
        decodeScript("fun f(x) { return match x { case 1 => 1 }; }\n"
                     "print 1;\n",
                     mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::vector<jvm::EmittedClass> classes =
        jvm::emitProgram(fn, tree, "LoxMain");

    ASSERT_EQ(classes.size(), 2u);
    const std::string& f = classes[1].source;
    // MATCH_ERROR: no operand, no successor this pass needs to reach — a
    // single call, like RETURN never falls through past it.
    EXPECT_NE(f.find("pop\n"
                     "    invokestatic lox/LoxOps/matchError()V\n"),
              std::string::npos)
        << f;
    expectEveryJumpTargetIsLabeled(f);
    expectEveryJumpTargetIsLabeled(classes[0].source);
}

// bytecode-translation-problems.md, "RETURN can return a named local, not
// only a temporary": examples/class_dispatch.lox's area()/describe() are
// this node's own checkpoint proof that this is reachable. Reverting to a
// bare `areturn` (no load), or loading `lastInvisibleVarSlot` instead of
// `localCount - 1` (this node's own first, wrong attempt — it loaded the
// Circle arm's own `r` binding, slot 8, instead of the match's result,
// slot 6), both make this test FAIL: the second arm's own binding
// ('color', ALSO slot 8, reused once the Circle arm's own binding is
// reclaimed) proves the two are not interchangeable.
TEST(EmitProgram, ReturnLoadsTheMatchResultNotTheLastArmBinding) {
    MemoryManager mm;
    DecodedFunction fn = decodeScript(
        "class Shape { init(color) { this.color = color; } }\n"
        "class Circle < Shape {\n"
        "  init(color, r) { super.init(color); this.r = r; }\n"
        "}\n"
        "fun area(s) {\n"
        "  return match s { case Circle{r} => r case Shape{color} => 0 };\n"
        "}\n"
        "print area(Circle(\"red\", 5));\n",
        mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    std::vector<jvm::EmittedClass> classes =
        jvm::emitProgram(fn, tree, "LoxMain");

    // LoxMain, Shape.init, Circle.init, area — in pre-order declaration
    // order.
    ASSERT_EQ(classes.size(), 4u);
    const std::string& area = classes[3].source;
    EXPECT_NE(area.find("aload 6\n"
                        "    areturn\n"),
              std::string::npos)
        << area;
    // The wrong slot this bug actually produced, so a future regression
    // that reintroduces it fails loudly rather than by coincidence.
    EXPECT_EQ(area.find("aload 8\n"
                        "    areturn\n"),
              std::string::npos)
        << area;
    expectEveryJumpTargetIsLabeled(area);
    for (const jvm::EmittedClass& cls : classes) {
        expectEveryJumpTargetIsLabeled(cls.source);
    }
}
