// test_backend_qbe_emitter.cpp — QBE straight-line + jumps emitter (S3,
// issue #456, notes/qbe-backend.md).
//
// Checkpoint: probes 01-05 and 15 must be byte-identical to native once
// assembled and run through the real toolchain
// (tools/check_qbe_s3_straight_line.sh) — that needs `qbe`, only available
// in the dev-qbe container, so it is not a ctest here. This file covers
// what a plain C++ unit test can check without it: the emitted .ssa's
// structural shape (one exported function, a labeled block per leader, a
// slot address per height), and that every opcode this node does not lower
// throws, naming itself, rather than silently emitting nothing.

#include "backend/abstract_stack.h"
#include "backend/chunk_decoder.h"
#include "backend/qbe_emitter.h"
#include "compiler.h"
#include "memory_manager.h"
#include "object.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace {

std::string emitScriptFrom(const std::string& source) {
    MemoryManager mm;
    ObjFunction* script = compile(source, &mm);
    if (script == nullptr) {
        throw std::runtime_error("compilation failed");
    }
    DecodedFunction tree = decodeFunctionTree(script);
    FunctionStackAnalysis analysis = analyzeStack(tree);
    return qbe::emitScript(tree, analysis, "lox_fn_0");
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

} // namespace

TEST(QbeEmitter, EmitsOneExportedFunctionMatchingRtCompiledFnShape) {
    std::string ssa = emitScriptFrom("print 1;");
    EXPECT_NE(ssa.find("export function w $lox_fn_0(l %rt, l %base) {"),
              std::string::npos);
    EXPECT_EQ(countOccurrences(ssa, "export function"), 1);
}

TEST(QbeEmitter, ConstantEmbedsRawDoubleBitsAtItsOwnHeight) {
    // CONSTANT 1.0 at height 1 (slot 0 is the script closure itself) ->
    // base+8, bit pattern 0x3FF0000000000000 = 4607182418800017408.
    std::string ssa = emitScriptFrom("print 1;");
    EXPECT_NE(ssa.find("storel 4607182418800017408"), std::string::npos);
}

TEST(QbeEmitter, EveryBlockEndsInATerminator) {
    // A block with no jmp/jnz/ret is invalid QBE input — every `@label`
    // line must be followed, before the next `@label` or the closing `}`,
    // by one of the three terminator forms.
    std::string ssa =
        emitScriptFrom("var i = 0; while (i < 3) { print i; i = i + 1; }");
    std::istringstream lines(ssa);
    std::string line;
    bool sawTerminatorSinceLabel = true;
    int labelCount = 0;
    while (std::getline(lines, line)) {
        if (!line.empty() && line[0] == '@') {
            EXPECT_TRUE(sawTerminatorSinceLabel)
                << "block before '" << line << "' has no terminator";
            sawTerminatorSinceLabel = false;
            labelCount++;
            continue;
        }
        if (line.find("jmp ") != std::string::npos ||
            line.find("jnz ") != std::string::npos ||
            line.find("ret ") != std::string::npos) {
            sawTerminatorSinceLabel = true;
        }
    }
    EXPECT_TRUE(sawTerminatorSinceLabel) << "final block has no terminator";
    EXPECT_GT(labelCount, 1) << "a while loop must produce more than one block";
}

TEST(QbeEmitter, JumpIfFalseBranchesOnBothTargets) {
    std::string ssa = emitScriptFrom("if (true) print 1; else print 2;");
    EXPECT_NE(ssa.find("jnz "), std::string::npos);
}

TEST(QbeEmitter, GlobalNameIsEmbeddedAsAnInternedDataString) {
    std::string ssa = emitScriptFrom("var g = 1; print g;");
    EXPECT_NE(ssa.find("data $lox_fn_0_name"), std::string::npos);
    EXPECT_NE(ssa.find("b \"g\""), std::string::npos);
    EXPECT_NE(ssa.find("call $rt_op_define_global"), std::string::npos);
    EXPECT_NE(ssa.find("call $rt_op_get_global"), std::string::npos);
}

TEST(QbeEmitter, ArithmeticHasAFastAndSlowPath) {
    std::string ssa = emitScriptFrom("print 1 + 2;");
    EXPECT_NE(ssa.find("call $rt_op_add"), std::string::npos);
    EXPECT_NE(ssa.find(" =d add "), std::string::npos);
}

TEST(QbeEmitter, ModuloHasNoInlineFastPath) {
    // Q5: MODULO's floor-division rule is not a plain double-double op —
    // always the slow path, never an inline `rem`/`div`+`mul`+`sub` shape.
    std::string ssa = emitScriptFrom("print 5 % 2;");
    EXPECT_NE(ssa.find("call $rt_op_modulo"), std::string::npos);
    EXPECT_EQ(ssa.find(" =d rem "), std::string::npos);
}

TEST(QbeEmitter, ReturnSetsTopAndReturnsZero) {
    std::string ssa = emitScriptFrom("print 1;");
    EXPECT_NE(ssa.find("call $rt_set_top"), std::string::npos);
    EXPECT_NE(ssa.find("ret 0"), std::string::npos);
}

// Proves the checks below can fail: each names an opcode genuinely outside
// this node's scope (S4 calls/closures, S5 the rest of the language, S6
// errors), and the emitter must say so rather than emit silently wrong
// code for it.

TEST(QbeEmitter, ThrowsNamingAnUnsupportedOpcode) {
    // clock() is a stdlib native reached through GET_GLOBAL/CALL with no
    // CLOSURE involved — CALL itself is what this test isolates (S4, #457).
    EXPECT_THROW(
        {
            try {
                emitScriptFrom("clock();");
            } catch (const std::exception& e) {
                EXPECT_NE(std::string(e.what()).find("CALL"),
                          std::string::npos);
                throw;
            }
        },
        std::runtime_error);
}

TEST(QbeEmitter, ThrowsOnPushHandler) {
    EXPECT_THROW(emitScriptFrom("try { throw 1; } catch (e) { print e; }"),
                 std::runtime_error);
}

TEST(QbeEmitter, ThrowsOnNonNumberConstant) {
    EXPECT_THROW(
        {
            try {
                emitScriptFrom("print \"hi\";");
            } catch (const std::exception& e) {
                EXPECT_NE(std::string(e.what()).find("CONSTANT"),
                          std::string::npos);
                throw;
            }
        },
        std::runtime_error);
}
