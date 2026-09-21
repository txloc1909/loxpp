// test_handler_depth_invariant.cpp — enforces the per-position rule:
// at every reachable bytecode offset, the compiler's m_openHandlerCount
// equals the runtime handler stack depth when control reaches it.
//
// Two legs: a static pass (analyzeHandlerDepth) over decoded chunks for
// every offset of every function, and a dynamic trace that checks each
// executed step of single-chunk programs against the static result.

#include "backend/chunk_decoder.h"
#include "backend/handler_depth.h"
#include "compiler.h"
#include "memory_manager.h"
#include "object.h"
#include "test_harness.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef LOXPP_PROJECT_SOURCE_DIR
#error                                                                         \
    "LOXPP_PROJECT_SOURCE_DIR must be defined by the build (see test/CMakeLists.txt)"
#endif

namespace {

namespace fs = std::filesystem;

fs::path projectRoot() { return fs::path(LOXPP_PROJECT_SOURCE_DIR); }

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + path.string());
    }
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

DecodedFunction decodeSource(const std::string& source, MemoryManager& mm) {
    ObjFunction* script = compile(source, &mm);
    if (script == nullptr) {
        throw std::runtime_error("compilation failed");
    }
    return decodeFunctionTree(script);
}

DecodedInstruction makeIns(int offset, Op op, int jumpTarget = -1) {
    DecodedInstruction in;
    in.offset = offset;
    in.op = op;
    in.jumpTarget = jumpTarget;
    return in;
}

// Static leg over one tree: every function analyzes with no inconsistency.
// A merge disagreement throws inside the pass, so a walk that completes
// has checked every merge in the program.
void checkTreeNoInconsistency(const DecodedFunction& node,
                              const std::string& path) {
    SCOPED_TRACE("function id=" + node.id + " path=" + path);
    analyzeHandlerDepth(node);
    for (const DecodedFunction& child : node.nested) {
        checkTreeNoInconsistency(child, path + " > " + child.displayName);
    }
}

void checkFileNoInconsistency(const fs::path& path) {
    SCOPED_TRACE("file=" + path.string());
    MemoryManager mm;
    DecodedFunction tree = decodeSource(readFile(path), mm);
    checkTreeNoInconsistency(tree, path.filename().string());
}

// Dynamic leg for single-chunk programs (no fun, no calls): every traced
// step must name a real offset and match the static depth there.
void checkTraceMatchesStatic(const std::string& source,
                             const std::string& expectedLog) {
    MemoryManager mm;
    DecodedFunction script = decodeSource(source, mm);
    EXPECT_TRUE(script.nested.empty())
        << "trace test programs must be single-chunk";
    HandlerDepthAnalysis analysis = analyzeHandlerDepth(script);
    std::unordered_map<int, int> expected;
    for (size_t i = 0; i < script.instructions.size(); i++) {
        if (analysis.reached[i]) {
            expected[script.instructions[i].offset] = analysis.before[i];
        }
    }

    VMTestHarness h;
    InterpretResult result = InterpretResult::RUNTIME_ERROR;
    std::vector<std::pair<int, int>> trace =
        h.runWithHandlerTrace(source, &result);
    EXPECT_EQ(result, InterpretResult::OK);
    EXPECT_EQ(h.handlerStackDepth(), 0);
    EXPECT_EQ(h.getGlobalStr("log"), expectedLog);
    ASSERT_FALSE(trace.empty()) << "trace recorded no steps";
    for (const auto& [offset, depth] : trace) {
        auto it = expected.find(offset);
        ASSERT_TRUE(it != expected.end())
            << "traced offset " << offset << " is not a reached instruction";
        EXPECT_EQ(depth, it->second) << "offset " << offset;
    }
}

} // namespace

TEST(HandlerDepthTest, BalancedTryCatchDepths) {
    // try body falls through to POP; catch path skips it.
    std::vector<DecodedInstruction> ins = {
        makeIns(0, Op::PUSH_HANDLER, 3),
        makeIns(1, Op::NIL),
        makeIns(2, Op::JUMP, 5),
        makeIns(3, Op::NIL),
        makeIns(4, Op::JUMP, 6),
        makeIns(5, Op::POP_HANDLER),
        makeIns(6, Op::NIL),
    };
    HandlerDepthAnalysis a = analyzeHandlerDepthIns(ins, "balanced");
    EXPECT_EQ(a.before[0], 0);
    EXPECT_EQ(a.after[0], 1);
    EXPECT_EQ(a.before[1], 1);
    EXPECT_EQ(a.before[2], 1);
    // Catch entry holds no record: THROW removed it.
    EXPECT_EQ(a.before[3], 0);
    EXPECT_EQ(a.before[4], 0);
    EXPECT_EQ(a.before[5], 1);
    EXPECT_EQ(a.after[5], 0);
    // Both paths agree at the merge.
    EXPECT_EQ(a.before[6], 0);
}

TEST(HandlerDepthTest, OrphanPopThrows) {
    std::vector<DecodedInstruction> ins = {
        makeIns(0, Op::NIL),
        makeIns(1, Op::POP_HANDLER),
    };
    EXPECT_THROW(analyzeHandlerDepthIns(ins, "orphan"), std::runtime_error);
}

TEST(HandlerDepthTest, ExtraPopOnCatchPathThrows) {
    // The #286 defect shape: one POP too many after catch entry.
    std::vector<DecodedInstruction> ins = {
        makeIns(0, Op::PUSH_HANDLER, 3),
        makeIns(1, Op::NIL),
        makeIns(2, Op::JUMP, 5),
        makeIns(3, Op::POP_HANDLER),
        makeIns(4, Op::JUMP, 6),
        makeIns(5, Op::POP_HANDLER),
        makeIns(6, Op::NIL),
    };
    EXPECT_THROW(analyzeHandlerDepthIns(ins, "extra-pop"), std::runtime_error);
}

TEST(HandlerDepthTest, MergeDisagreementThrows) {
    // Only one branch opens a region before the merge.
    std::vector<DecodedInstruction> ins = {
        makeIns(0, Op::JUMP_IF_FALSE, 3),
        makeIns(1, Op::PUSH_HANDLER, 5),
        makeIns(2, Op::JUMP, 4),
        makeIns(3, Op::NIL),
        makeIns(4, Op::NIL),
        makeIns(5, Op::NIL),
    };
    EXPECT_THROW(analyzeHandlerDepthIns(ins, "merge"), std::runtime_error);
}

TEST(HandlerDepthTest, SharedCatchAtSameDepthAgrees) {
    // Two regions may name one catch entry in hand-built input. Both
    // seeds state depth 0, so the pass completes.
    std::vector<DecodedInstruction> ins = {
        makeIns(0, Op::PUSH_HANDLER, 5),
        makeIns(1, Op::POP_HANDLER),
        makeIns(2, Op::PUSH_HANDLER, 5),
        makeIns(3, Op::POP_HANDLER),
        makeIns(4, Op::NIL),
        makeIns(5, Op::NIL),
    };
    HandlerDepthAnalysis a = analyzeHandlerDepthIns(ins, "shared-agree");
    EXPECT_EQ(a.before[5], 0);
}

TEST(HandlerDepthTest, SharedCatchAtOtherDepthThrows) {
    // The second seed states depth 1 against depth 0: disagreement.
    std::vector<DecodedInstruction> ins = {
        makeIns(0, Op::PUSH_HANDLER, 5),
        makeIns(1, Op::PUSH_HANDLER, 5),
        makeIns(2, Op::POP_HANDLER),
        makeIns(3, Op::POP_HANDLER),
        makeIns(4, Op::NIL),
        makeIns(5, Op::NIL),
    };
    EXPECT_THROW(analyzeHandlerDepthIns(ins, "shared-disagree"),
                 std::runtime_error);
}

TEST(HandlerDepthTest, BalancedLoopBackEdge) {
    std::vector<DecodedInstruction> ins = {
        makeIns(0, Op::NIL),
        makeIns(1, Op::JUMP_IF_FALSE, 6),
        makeIns(2, Op::PUSH_HANDLER, 5),
        makeIns(3, Op::POP_HANDLER),
        makeIns(4, Op::LOOP, 1),
        makeIns(5, Op::NIL),
        makeIns(6, Op::NIL),
    };
    HandlerDepthAnalysis a = analyzeHandlerDepthIns(ins, "loop");
    EXPECT_EQ(a.before[2], 0);
    EXPECT_EQ(a.after[2], 1);
    EXPECT_EQ(a.before[3], 1);
    EXPECT_EQ(a.before[5], 0);
    EXPECT_EQ(a.before[6], 0);
}

TEST(HandlerDepthTest, ContinueFromCatchMatchesStaticDepths) {
    checkTraceMatchesStatic(
        R"(
        var log = "";
        var i = 0;
        while (i < 2) {
            try { throw "boom"; } catch (a) { log = log + "first;"; i = i + 1; continue; }
            try { log = log + "second;"; } catch (b) { log = log + "never;"; }
            i = i + 1;
        }
    )",
        "first;first;");
}

TEST(HandlerDepthTest, BreakFromCatchMatchesStaticDepths) {
    checkTraceMatchesStatic(
        R"(
        var log = "";
        for (var i = 0; i < 3; i = i + 1) {
            try { throw "boom"; } catch (e) { log = log + "catch;"; break; }
            log = log + "unreached;";
        }
        log = log + "done;";
    )",
        "catch;done;");
}

TEST(HandlerDepthTest, OuterTrySurvivesInnerCatchBreak) {
    checkTraceMatchesStatic(
        R"(
        var log = "";
        try {
            for (var i = 0; i < 2; i = i + 1) {
                try { throw "boom"; } catch (e) { log = log + "inner;"; break; }
            }
            throw "outer-test";
        } catch (e2) { log = log + "outer:" + e2 + ";"; }
    )",
        "inner;outer:outer-test;");
}

TEST(HandlerDepthTest, RunsOverEveryProbeWithNoInconsistency) {
    std::vector<fs::path> probes;
    for (const auto& entry : fs::directory_iterator(projectRoot() / "test" /
                                                    "translation-probes")) {
        if (entry.path().extension() == ".lox") {
            probes.push_back(entry.path());
        }
    }
    std::sort(probes.begin(), probes.end());
    ASSERT_FALSE(probes.empty()) << "no translation probes found";
    for (const fs::path& probe : probes) {
        checkFileNoInconsistency(probe);
    }
}

TEST(HandlerDepthTest, RunsOverEveryExampleWithNoInconsistency) {
    std::vector<fs::path> examples;
    for (const auto& entry :
         fs::directory_iterator(projectRoot() / "examples")) {
        if (entry.path().extension() == ".lox") {
            examples.push_back(entry.path());
        }
    }
    std::sort(examples.begin(), examples.end());
    ASSERT_FALSE(examples.empty()) << "no example programs found";
    for (const fs::path& example : examples) {
        checkFileNoInconsistency(example);
    }
}

TEST(HandlerDepthTest, RunsOverBootstrapInterpreterWithNoInconsistency) {
    checkFileNoInconsistency(projectRoot() / "bootstrap" /
                             "loxpp_interpreter.lox");
}
