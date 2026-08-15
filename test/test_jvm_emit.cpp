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

// N6.md (assigned nit, PR #109 R9): a `goto`/`ifne` to a jasmin label the
// emitter never wrote assembles fine as far as ctest can see — only
// tools/check_jvm_probes.sh (jasmin + java, container-only) would ever
// reject it. N5's own R1 was exactly this class of bug, on probe 22.
// Collects every operand of `goto ` and `ifne `, then asserts a "<name>:"
// line exists for each one, so a plain unit test in this file catches the
// same defect at zero runtime cost. Call at the end of every emitScript
// test; every node after N6 that adds a jump inherits the net for free.
void expectEveryJumpTargetIsLabeled(const std::string& j) {
    for (const std::string& mnemonic :
         {std::string("goto "), std::string("ifne ")}) {
        std::size_t pos = 0;
        while ((pos = j.find(mnemonic, pos)) != std::string::npos) {
            std::size_t nameStart = pos + mnemonic.size();
            std::size_t nameEnd = j.find('\n', nameStart);
            ASSERT_NE(nameEnd, std::string::npos)
                << mnemonic << "operand runs off the end of:\n"
                << j;
            std::string target = j.substr(nameStart, nameEnd - nameStart);
            EXPECT_NE(j.find(target + ":\n"), std::string::npos)
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
    // A class declaration compiles to CLASS — node N8's job, not N6's.
    DecodedFunction fn = decodeScript("class Foo {}", mm);
    FunctionStackAnalysis analysis = analyzeStack(fn);
    try {
        jvm::emitScript(fn, analysis, "LoxMain");
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        EXPECT_EQ(std::string(e.what()), "not implemented in N6: CLASS");
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

TEST(EmitProgram, ClosureWithUpvalueIsNotImplemented) {
    // 06_shared_upvalue: `get` captures `x`, so its CLOSURE carries one
    // upvalue entry. N6 only lowers the zero-upvalue construction; wiring a
    // real cell into it is node N7 (jvm_emitter.h hazard note).
    MemoryManager mm;
    DecodedFunction fn = decodeScript("fun outer() {\n"
                                      "  var x = 0;\n"
                                      "  fun get() { return x; }\n"
                                      "  return get;\n"
                                      "}\n",
                                      mm);
    StackAnalysisTree tree = analyzeStackTree(fn);
    try {
        jvm::emitProgram(fn, tree, "LoxMain");
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        EXPECT_EQ(std::string(e.what()),
                  "not implemented in N6: CLOSURE with 1 upvalue(s) (upvalue "
                  "wiring is node N7)");
    }
}
