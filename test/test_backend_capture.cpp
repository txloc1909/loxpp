// test_backend_capture.cpp — capture-analysis tests (node N3).
//
// The checkpoint (notes/backend-implementation-dag.md, node N3; the N3 node
// spec P4a section): the five ground-truth probes must each get the exact
// live-range / sharing verdict their disassembly settles, and the pass must
// not throw over the whole probe/example/bootstrap corpus.

#include "backend/capture_analysis.h"
#include "backend/chunk_decoder.h"
#include "compiler.h"
#include "memory_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
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

std::vector<fs::path> listLoxFiles(const fs::path& dir) {
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".lox") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

// Finds the first node, at any depth, whose displayName matches `name` — the
// probes below each declare only one function with the name under test.
const DecodedFunction* findByName(const DecodedFunction& node,
                                  const std::string& name) {
    if (node.displayName == name) {
        return &node;
    }
    for (const DecodedFunction& child : node.nested) {
        if (const DecodedFunction* found = findByName(child, name)) {
            return found;
        }
    }
    return nullptr;
}

// Bundles the compiled tree with the capture analysis and the MemoryManager
// that owns every ObjFunction the tree points at, so all three stay alive
// together for the length of a test.
struct Compiled {
    std::unique_ptr<MemoryManager> mm;
    DecodedFunction tree;
    CaptureAnalysis captures;
};

// Compiles `source`, decodes it (N0), and runs the capture pass (N3).
// Throws on a compile failure, matching test_chunk_decoder.cpp's
// convention: a corpus file that fails to compile is a corpus problem, not a
// soft-fail case.
Compiled compileAndAnalyze(const std::string& source,
                           const std::string& label) {
    Compiled result;
    result.mm = std::make_unique<MemoryManager>();
    ObjFunction* script = compile(source, result.mm.get());
    if (script == nullptr) {
        throw std::runtime_error("compilation failed for " + label);
    }
    result.tree = decodeFunctionTree(script);
    result.captures = analyzeCaptures(result.tree);
    return result;
}

Compiled compileFile(const fs::path& path) {
    return compileAndAnalyze(readFile(path), path.filename().string());
}

const FunctionCaptureInfo& infoFor(const CaptureAnalysis& captures,
                                   const std::string& id) {
    auto it = captures.functions.find(id);
    if (it == captures.functions.end()) {
        throw std::runtime_error("no capture info for function id=" + id);
    }
    return it->second;
}

const std::vector<CaptureLiveRange>& rangesFor(const FunctionCaptureInfo& info,
                                               int slot) {
    auto it = info.liveRangesBySlot.find(slot);
    if (it == info.liveRangesBySlot.end()) {
        throw std::runtime_error("slot " + std::to_string(slot) +
                                 " is not captured in function id=" + info.id);
    }
    return it->second;
}

} // namespace

TEST(CaptureAnalysisTest, SharedUpvalueOneCellNotTwo) {
    // 06_shared_upvalue.lox: get and set both capture outer's slot 1 --
    // "| local 1" on both CLOSURE instructions, with no CLOSE_UPVALUE
    // between them. Checkpoint: one shared cell, not one per closure.
    Compiled c = compileFile(projectRoot() / "notes" / "translation-probes" /
                             "06_shared_upvalue.lox");
    const DecodedFunction* outer = findByName(c.tree, "outer");
    ASSERT_NE(outer, nullptr);

    const FunctionCaptureInfo& info = infoFor(c.captures, outer->id);
    const std::vector<CaptureLiveRange>& ranges = rangesFor(info, 1);
    ASSERT_EQ(ranges.size(), 1U)
        << "slot 1 must have exactly one live range: both closures capture "
        << "it before either is closed";
    EXPECT_EQ(ranges[0].capturingClosureOffsets.size(), 2U)
        << "get and set must share the same live range";
    EXPECT_TRUE(ranges[0].closedImplicitly)
        << "a function's own top-level scope closes with the frame, with no "
        << "explicit CLOSE_UPVALUE";
}

TEST(CaptureAnalysisTest, FreshCellPerIterationForBodyLocal) {
    // V1_fresh_cell.lox: `snapshot` (slot 3) is declared inside the loop
    // body, and its CLOSE_UPVALUE (offset 49) sits before the back-edge
    // (LOOP at 50) -- the range is per-iteration, matching runtime output
    // 0, 1, 2.
    Compiled c = compileFile(projectRoot() / "notes" / "translation-probes" /
                             "V1_fresh_cell.lox");
    const DecodedFunction* make = findByName(c.tree, "make");
    ASSERT_NE(make, nullptr);

    const FunctionCaptureInfo& info = infoFor(c.captures, make->id);
    const std::vector<CaptureLiveRange>& ranges = rangesFor(info, 3);
    ASSERT_EQ(ranges.size(), 1U)
        << "the loop body is decoded once; one static live range must "
        << "represent every runtime iteration";
    EXPECT_TRUE(ranges[0].perIteration)
        << "snapshot's CLOSE_UPVALUE sits inside the loop body, before the "
        << "back-edge, so each iteration must get a fresh cell";
    EXPECT_FALSE(ranges[0].closedImplicitly);
    EXPECT_EQ(ranges[0].capturingClosureOffsets.size(), 1U);
}

TEST(CaptureAnalysisTest, SharedCellForWholeLoopVar) {
    // V3_loopvar.lox: `i` (slot 2, the loop variable itself) has no
    // CLOSE_UPVALUE inside the body; the only one is after the loop exits,
    // so every iteration shares one cell, matching runtime output 3, 3, 3.
    Compiled c = compileFile(projectRoot() / "notes" / "translation-probes" /
                             "V3_loopvar.lox");
    const DecodedFunction* make = findByName(c.tree, "make");
    ASSERT_NE(make, nullptr);

    const FunctionCaptureInfo& info = infoFor(c.captures, make->id);
    const std::vector<CaptureLiveRange>& ranges = rangesFor(info, 2);
    ASSERT_EQ(ranges.size(), 1U);
    EXPECT_FALSE(ranges[0].perIteration)
        << "i's only CLOSE_UPVALUE is after the loop's back-edge, so the "
        << "whole loop must share one cell";
    EXPECT_FALSE(ranges[0].closedImplicitly);
}

TEST(CaptureAnalysisTest, MutableSharedUpvalueOneCell) {
    // V2_shared.lox: get and inc both capture counter's slot 1; inc mutates
    // it and get observes the mutation (runtime prints 2), which only a
    // single shared cell can produce.
    Compiled c = compileFile(projectRoot() / "notes" / "translation-probes" /
                             "V2_shared.lox");
    const DecodedFunction* counter = findByName(c.tree, "counter");
    ASSERT_NE(counter, nullptr);

    const FunctionCaptureInfo& info = infoFor(c.captures, counter->id);
    const std::vector<CaptureLiveRange>& ranges = rangesFor(info, 1);
    ASSERT_EQ(ranges.size(), 1U);
    EXPECT_EQ(ranges[0].capturingClosureOffsets.size(), 2U);
    EXPECT_TRUE(ranges[0].closedImplicitly);
}

TEST(CaptureAnalysisTest, SuperIsAnOrdinaryCapture) {
    // 10_super.lox: the compiler synthesises a hidden `super` local in the
    // class-body scope, and B::greet captures it as an upvalue, closed by
    // an explicit CLOSE_UPVALUE when the class body ends. No special-casing
    // is needed: the analysis must see this exactly like any other capture.
    Compiled c = compileFile(projectRoot() / "notes" / "translation-probes" /
                             "10_super.lox");
    const FunctionCaptureInfo& script = infoFor(c.captures, c.tree.id);
    ASSERT_EQ(script.liveRangesBySlot.size(), 1U)
        << "the hidden super local is the only capture at script scope";
    const std::vector<CaptureLiveRange>& ranges =
        script.liveRangesBySlot.begin()->second;
    ASSERT_EQ(ranges.size(), 1U);
    EXPECT_EQ(ranges[0].capturingClosureOffsets.size(), 1U);
    EXPECT_FALSE(ranges[0].closedImplicitly)
        << "the class body's endScope emits an explicit CLOSE_UPVALUE";

    const DecodedFunction* greetB = nullptr;
    for (const DecodedFunction& child : c.tree.nested) {
        if (child.displayName == "greet" &&
            !infoFor(c.captures, child.id).ownUpvalues.empty()) {
            greetB = &child;
        }
    }
    ASSERT_NE(greetB, nullptr)
        << "B::greet is the only 'greet' that captures an upvalue";
    const std::vector<ClosureUpvalue>& wiring =
        infoFor(c.captures, greetB->id).ownUpvalues;
    ASSERT_EQ(wiring.size(), 1U);
    EXPECT_TRUE(wiring[0].isLocal)
        << "super is wired as a parent LOCAL capture, not a forwarded "
        << "upvalue";
}

// Runs the pass over one file and checks it never throws, and that every
// live range it reports is internally consistent: non-negative, non-empty,
// backed by at least one capturing closure, and non-overlapping with its
// slot's other ranges.
void checkNoAssertionFailure(const fs::path& path) {
    SCOPED_TRACE("file=" + path.string());
    Compiled c = compileFile(path);
    for (const auto& [id, info] : c.captures.functions) {
        for (const auto& [slot, ranges] : info.liveRangesBySlot) {
            int previousEnd = -1;
            for (const CaptureLiveRange& range : ranges) {
                EXPECT_GE(range.start, 0) << "id=" << id << " slot=" << slot;
                EXPECT_GE(range.end, range.start)
                    << "id=" << id << " slot=" << slot;
                EXPECT_FALSE(range.capturingClosureOffsets.empty())
                    << "id=" << id << " slot=" << slot
                    << ": a live range must have at least one capturing "
                    << "closure, or it would never have opened";
                EXPECT_GE(range.start, previousEnd)
                    << "id=" << id << " slot=" << slot
                    << ": live ranges of one slot must not overlap";
                previousEnd = range.end;
            }
        }
    }
}

TEST(CaptureAnalysisTest, NoAssertionFailureOnTranslationProbes) {
    std::vector<fs::path> probes =
        listLoxFiles(projectRoot() / "notes" / "translation-probes");
    ASSERT_FALSE(probes.empty()) << "no translation probes found";
    for (const fs::path& probe : probes) {
        checkNoAssertionFailure(probe);
    }
}

TEST(CaptureAnalysisTest, NoAssertionFailureOnExamples) {
    std::vector<fs::path> examples = listLoxFiles(projectRoot() / "examples");
    ASSERT_FALSE(examples.empty()) << "no example programs found";
    for (const fs::path& example : examples) {
        checkNoAssertionFailure(example);
    }
}

TEST(CaptureAnalysisTest, NoAssertionFailureOnBootstrapInterpreter) {
    checkNoAssertionFailure(projectRoot() / "bootstrap" /
                            "loxpp_interpreter.lox");
}
