// test_backend_capture.cpp — capture-analysis tests (node N3).
//
// The checkpoint (notes/backend-implementation-dag.md, node N3; the N3 node
// spec P4a section): the five ground-truth probes must each get the exact
// live-range / sharing verdict their disassembly settles, and the pass must
// not throw over the whole probe/example/bootstrap corpus.
//
// checkNoAssertionFailure additionally cross-checks every corpus file's
// reported ranges against its own decoded CLOSE_UPVALUE offsets (not just
// the analysis's own output) — see checkCloseUpvaluesMatchDecodedChunk.
// MatchArmBindingAndLaterBlockLocalDoNotCrossAttribute,
// LoopVarRangeDoesNotDominateItsOwnEnd, and
// PerIterationCaptureClosesOnBreakAndContinue pin down three review-round-1
// findings (R1/R2, R3, R5) as regression tests.

#include "backend/capture_analysis.h"
#include "backend/chunk_decoder.h"
#include "compiler.h"
#include "memory_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
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

// R1/R2 regression: a match-arm pattern binding and a later, unrelated block
// local must each close to their OWN slot, never cross-attributed. Before
// Compiler::emitLoopCleanup was fixed to check isCaptured (see
// fix/close-captured-locals-on-early-exit), a captured match-arm binding left
// scope with a plain POP, so its slot stayed open; a later CLOSE_UPVALUE for
// an unrelated captured block local then closed the WRONG (higher, stale)
// slot, and the real target was left reporting closedImplicitly=true out to
// the chunk's end instead of its own real CLOSE_UPVALUE.
TEST(CaptureAnalysisTest,
     MatchArmBindingAndLaterBlockLocalDoNotCrossAttribute) {
    Compiled c = compileAndAnalyze(R"(
        enum Result { Ok(value) Err(msg) }
        fun make() {
            var box = [nil, nil];
            var r = Ok(7);
            var g = match r {
                case Ok(v) => { fun f() { return v; } box[0] = f; 1 }
                case Err(m) => 0
            };
            {
                var w = 9;
                fun gw() { return w; }
                box[1] = gw;
            }
            return box;
        }
    )",
                                   "match_arm_cross_attribution");
    const DecodedFunction* make = findByName(c.tree, "make");
    ASSERT_NE(make, nullptr);
    const FunctionCaptureInfo& info = infoFor(c.captures, make->id);

    const DecodedFunction* f = findByName(c.tree, "f");
    const DecodedFunction* gw = findByName(c.tree, "gw");
    ASSERT_NE(f, nullptr);
    ASSERT_NE(gw, nullptr);
    int vSlot = infoFor(c.captures, f->id).ownUpvalues.at(0).index;
    int wSlot = infoFor(c.captures, gw->id).ownUpvalues.at(0).index;
    ASSERT_NE(vSlot, wSlot);

    const std::vector<CaptureLiveRange>& vRanges = rangesFor(info, vSlot);
    const std::vector<CaptureLiveRange>& wRanges = rangesFor(info, wSlot);
    ASSERT_EQ(vRanges.size(), 1U);
    ASSERT_EQ(wRanges.size(), 1U);

    EXPECT_FALSE(vRanges[0].closedImplicitly)
        << "the match arm's own exit closes v with a real CLOSE_UPVALUE";
    EXPECT_FALSE(wRanges[0].closedImplicitly)
        << "the later block's own endScope closes w with a real "
        << "CLOSE_UPVALUE";
    EXPECT_LT(vRanges[0].end, wRanges[0].firstCaptureOffset)
        << "v must close before w is even captured -- the two ranges must "
        << "not overlap or share an end offset";
}

// R3 documentation: `firstCaptureOffset` is a bound, not the declaration
// offset, and it does not dominate `end`. V3_loopvar.lox is the standing
// counter-example: a zero-trip loop runs the CLOSE_UPVALUE that ends `i`'s
// range without ever running the CLOSURE that opened it (the declaration,
// the init clause's `var i = 0`, runs once before the loop and is not
// reflected in either field). See capture_analysis.h for what this means for
// N7. This test pins the exact counter-example down as an intentional,
// tested contract, not a silent gap.
TEST(CaptureAnalysisTest, LoopVarRangeDoesNotDominateItsOwnEnd) {
    Compiled c = compileFile(projectRoot() / "notes" / "translation-probes" /
                             "V3_loopvar.lox");
    const DecodedFunction* make = findByName(c.tree, "make");
    ASSERT_NE(make, nullptr);
    const FunctionCaptureInfo& info = infoFor(c.captures, make->id);
    const std::vector<CaptureLiveRange>& ranges = rangesFor(info, 2);
    ASSERT_EQ(ranges.size(), 1U);

    // The JUMP_IF_FALSE that skips the whole loop body on a zero-trip run
    // must jump to a target that is > firstCaptureOffset (inside the
    // range) and <= end -- i.e. some path reaches the range without ever
    // running the instruction firstCaptureOffset names.
    bool foundSkipIntoRange = false;
    for (const DecodedInstruction& ins : make->instructions) {
        if (ins.op == Op::JUMP_IF_FALSE &&
            ins.offset < ranges[0].firstCaptureOffset &&
            ins.jumpTarget > ranges[0].firstCaptureOffset &&
            ins.jumpTarget <= ranges[0].end) {
            foundSkipIntoRange = true;
        }
    }
    EXPECT_TRUE(foundSkipIntoRange)
        << "a zero-trip loop must be able to reach this range without "
        << "running firstCaptureOffset -- if this ever stops being true, "
        << "update this test and the header doc together";
}

// R5 regression: a per-iteration captured local must close correctly even
// when the loop body has more than one exit path (continue, break, and the
// normal fall-through). Before the compiler fix, only the fall-through path
// closed with CLOSE_UPVALUE; break and continue used a plain POP, so the
// capture never showed up as closed on those paths, and the analysis had no
// signal that anything was wrong.
TEST(CaptureAnalysisTest, PerIterationCaptureClosesOnBreakAndContinue) {
    Compiled c = compileAndAnalyze(R"(
        fun make() {
            var fns = [nil, nil, nil];
            for (var i = 0; i < 3; i = i + 1) {
                var s = i;
                fun f() { return s; }
                fns[i] = f;
                if (i == 1) continue;
                if (i == 2) break;
            }
            return fns;
        }
    )",
                                   "break_continue_capture");
    const DecodedFunction* make = findByName(c.tree, "make");
    const DecodedFunction* f = findByName(c.tree, "f");
    ASSERT_NE(make, nullptr);
    ASSERT_NE(f, nullptr);
    const FunctionCaptureInfo& info = infoFor(c.captures, make->id);
    int sSlot = infoFor(c.captures, f->id).ownUpvalues.at(0).index;

    const std::vector<CaptureLiveRange>& ranges = rangesFor(info, sSlot);
    ASSERT_EQ(ranges.size(), 1U)
        << "the loop body is decoded once; one static range represents "
        << "every runtime iteration, break/continue included";
    EXPECT_TRUE(ranges[0].perIteration)
        << "s is declared inside the loop body, so every iteration -- "
        << "however it exits -- needs a fresh cell";
    EXPECT_FALSE(ranges[0].closedImplicitly);
}

// Regression: an unrelated capture that fully opens and closes BETWEEN two
// of a loop-var capture's own alternate exit points must not confuse the
// tolerance in recordClose. Here `t` is captured and closed, in its own
// block, strictly between the `continue` path's close of `s` and the
// `break` path's second (tolerated) close of the same `s` range -- so by
// the time the break path's CLOSE_UPVALUE runs, the most recently closed
// slot is `t`, not `s`. The pass must still not throw: it never tries to
// verify that a tolerated close belongs to any particular slot (see
// recordClose), so this shape is safe by construction, not by luck.
TEST(CaptureAnalysisTest, ToleratesUnrelatedCaptureBetweenAlternateExits) {
    Compiled c = compileAndAnalyze(R"(
        fun make() {
            var fns = [nil, nil, nil];
            for (var i = 0; i < 3; i = i + 1) {
                var s = i;
                fun f() { return s; }
                fns[i] = f;
                if (i == 1) continue;
                {
                    var t = i * 10;
                    fun g() { return t; }
                    if (i == 0) { print g(); }
                }
                if (i == 2) break;
            }
            return fns;
        }
    )",
                                   "unrelated_capture_between_exits");
    const DecodedFunction* make = findByName(c.tree, "make");
    const DecodedFunction* f = findByName(c.tree, "f");
    ASSERT_NE(make, nullptr);
    ASSERT_NE(f, nullptr);
    const FunctionCaptureInfo& info = infoFor(c.captures, make->id);
    int sSlot = infoFor(c.captures, f->id).ownUpvalues.at(0).index;

    const std::vector<CaptureLiveRange>& ranges = rangesFor(info, sSlot);
    ASSERT_EQ(ranges.size(), 1U);
    EXPECT_TRUE(ranges[0].perIteration);
    EXPECT_FALSE(ranges[0].closedImplicitly);
}

// Collects every (id -> DecodedFunction*) pair in `root`'s tree, so a check
// can look up the decoded instructions that back one FunctionCaptureInfo.
void collectById(const DecodedFunction& node,
                 std::map<std::string, const DecodedFunction*>& out) {
    out[node.id] = &node;
    for (const DecodedFunction& child : node.nested) {
        collectById(child, out);
    }
}

// R4: the corpus test must not only check the analysis output in isolation —
// it must cross-check it against the decoded chunk that produced it, or a
// misattributed CLOSE_UPVALUE (R1) passes every other check here silently.
void checkCloseUpvaluesMatchDecodedChunk(const DecodedFunction& node,
                                         const FunctionCaptureInfo& info) {
    std::set<int> decodedCloses;
    for (const DecodedInstruction& ins : node.instructions) {
        if (ins.op == Op::CLOSE_UPVALUE) {
            decodedCloses.insert(ins.offset);
        }
    }

    // A range can have more than one real CLOSE_UPVALUE backing it: break,
    // continue, and a match arm's own exit each clean up their scope
    // independently, so one captured local can close on any of several
    // mutually exclusive paths (see capture_analysis.cpp's recordClose).
    // The pass keeps only the first one it sees in program order, so this
    // checks a subset, not a one-to-one match: every reported `end` must be
    // a real CLOSE_UPVALUE in the chunk, and the chunk must have at least as
    // many CLOSE_UPVALUE instructions as non-implicit ranges — never fewer.
    int nonImplicitRangeCount = 0;
    for (const auto& [slot, ranges] : info.liveRangesBySlot) {
        for (const CaptureLiveRange& range : ranges) {
            if (range.closedImplicitly) {
                continue;
            }
            nonImplicitRangeCount++;
            EXPECT_TRUE(decodedCloses.count(range.end) > 0)
                << "id=" << info.id << " slot=" << slot
                << ": reported end=" << range.end
                << " is not a real CLOSE_UPVALUE offset in the chunk";
        }
    }
    EXPECT_GE(decodedCloses.size(), static_cast<size_t>(nonImplicitRangeCount))
        << "id=" << info.id
        << ": fewer CLOSE_UPVALUE instructions than non-implicit ranges — "
        << "some range's close was never actually decoded";
}

// Runs the pass over one file and checks it never throws, and that every
// live range it reports is internally consistent: non-negative, non-empty,
// backed by at least one capturing closure, non-overlapping with its slot's
// other ranges, and matches the decoded chunk's own CLOSE_UPVALUE offsets
// (R4 — the oracle is the chunk, not the analysis's own output).
void checkNoAssertionFailure(const fs::path& path) {
    SCOPED_TRACE("file=" + path.string());
    Compiled c = compileFile(path);
    std::map<std::string, const DecodedFunction*> byId;
    collectById(c.tree, byId);

    for (const auto& [id, info] : c.captures.functions) {
        for (const auto& [slot, ranges] : info.liveRangesBySlot) {
            int previousEnd = -1;
            for (const CaptureLiveRange& range : ranges) {
                EXPECT_GE(range.firstCaptureOffset, 0)
                    << "id=" << id << " slot=" << slot;
                EXPECT_GE(range.end, range.firstCaptureOffset)
                    << "id=" << id << " slot=" << slot;
                EXPECT_FALSE(range.capturingClosureOffsets.empty())
                    << "id=" << id << " slot=" << slot
                    << ": a live range must have at least one capturing "
                    << "closure, or it would never have opened";
                EXPECT_GE(range.firstCaptureOffset, previousEnd)
                    << "id=" << id << " slot=" << slot
                    << ": live ranges of one slot must not overlap";
                previousEnd = range.end;
            }
        }
        checkCloseUpvaluesMatchDecodedChunk(*byId.at(id), info);
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
