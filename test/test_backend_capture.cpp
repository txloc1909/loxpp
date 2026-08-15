// test_backend_capture.cpp — capture-analysis tests (node N3).
//
// The checkpoint (notes/backend-implementation-dag.md, node N3; the N3 node
// spec P4a section): the five ground-truth probes must each get the exact
// live-range / sharing verdict their disassembly settles, and the pass must
// not throw over the whole probe/example/bootstrap corpus.
//
// validateCaptureAnalysis cross-checks every reported range against its own
// decoded CLOSE_UPVALUE offsets in both directions (not just the analysis's
// own output) — see checkCloseUpvaluesMatchDecodedChunk and
// checkNoOrphanCloseUpvalues. compileAndAnalyze calls it on every result
// before returning, so it runs over the corpus (probes, examples, bootstrap
// interpreter) AND over every targeted regression test below, with no way
// for a test to skip it (round-3 referee decision, item 4): a check that
// only ever sees programs with zero orphans to find proves nothing about
// the defect it exists to catch.
// MatchArmBindingAndLaterBlockLocalDoNotCrossAttribute,
// LoopVarRangeDoesNotDominateItsOwnEnd, and
// PerIterationCaptureClosesOnBreakAndContinue pin down three review-round-1
// findings (R1/R2, R3, R5) as regression tests.
// LoopVarNotClosedEarlyByAlternateExit, OuterCaptureNotClosedEarlyByLoopExit,
// and RecapturedSlotAfterDeadEarlyExitSharesOneCell pin down review-round-2
// findings R9 and R10: a captured slot with more than one real
// CLOSE_UPVALUE in the same chunk (break/continue/fall-through each
// cleaning up their own path) must not let a later, unrelated close, or a
// later re-capture, resolve to the wrong slot or split one runtime cell into
// two.
// PerIterationCaptureInsideIfInsideLoop (R13) and
// DifferentVariablesReusingOneSlotDoNotShareACell (R14) pin down two
// review-round-3 findings, resolved by the round-3 referee decision: the
// pass now derives live ranges per CFG execution path (see analyzeOneChunk
// in capture_analysis.cpp), not a JUMP_IF_FALSE span heuristic over a flat
// instruction walk, so a capture's verdict depends only on the CFG paths
// that actually reach it.

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

// Collects every (id -> DecodedFunction*) pair in root's tree, so a check
// can look up the decoded instructions that back one FunctionCaptureInfo.
void collectById(const DecodedFunction& node,
                 std::map<std::string, const DecodedFunction*>& out) {
    out[node.id] = &node;
    for (const DecodedFunction& child : node.nested) {
        collectById(child, out);
    }
}

// R4 / R9 / R15 / round-3 referee decision item 4: cross-checks every
// reported range against the decoded chunk's own CLOSE_UPVALUE offsets, in
// both directions -- a misattributed close (R1, R9) must not pass silently.
//
// Round 1's version of this check compared counts (decodedCloses.size() >=
// nonImplicitRangeCount). That passed on the R9 program: the pass reported
// one CLOSE_UPVALUE too many for the loop var's range (misattributing a
// second alternate-exit close meant for a different slot) and, separately,
// never attributed the loop var's own real close at all -- two errors that
// canceled out in a size comparison. A count can hide exactly the defect
// this check exists to catch, so it checks membership in both directions
// instead: every reported close offset must be a real CLOSE_UPVALUE in the
// chunk (below), AND every real CLOSE_UPVALUE in the chunk must be one
// reported (checkNoOrphanCloseUpvalues), or listed as unreachable dead code.
// Both directions check the FULL allCloseOffsets set, not only end -- R9's
// and R10's ground-truth programs each have a range with two real alternate
// closes, and checking end alone would call the earlier one an orphan.
void checkCloseUpvaluesMatchDecodedChunk(const DecodedFunction& node,
                                         const FunctionCaptureInfo& info) {
    std::set<int> decodedCloses;
    for (const DecodedInstruction& ins : node.instructions) {
        if (ins.op == Op::CLOSE_UPVALUE) {
            decodedCloses.insert(ins.offset);
        }
    }
    for (const auto& [slot, ranges] : info.liveRangesBySlot) {
        for (const CaptureLiveRange& range : ranges) {
            for (int offset : range.allCloseOffsets) {
                EXPECT_TRUE(decodedCloses.contains(offset))
                    << "id=" << info.id << " slot=" << slot
                    << ": reported close offset=" << offset
                    << " is not a real CLOSE_UPVALUE offset in the chunk";
            }
        }
    }
}

// R4 / R9 / R15 / referee amendment 1 item 4: the other direction of the
// cross-check above, strengthened to count occurrences, not just presence. A
// CLOSE_UPVALUE the chunk actually contains, but that matches no reported
// range's close offsets and is not listed as unreachable dead code, is
// exactly the R9 shape: the close was seen and misattributed elsewhere, with
// no other check here noticing. A CLOSE_UPVALUE reported more than once
// (double-attributed to two ranges, or both a range and "unreachable") is
// just as wrong, so every real close must appear EXACTLY once across the
// whole report. This needs no CFG and no stack simulation of its own -- only
// the analysis's own reported closes, compared against the chunk it read.
void checkNoOrphanCloseUpvalues(const DecodedFunction& node,
                                const FunctionCaptureInfo& info) {
    std::map<int, int> occurrences;
    for (int offset : info.unreachableCloseOffsets) {
        occurrences[offset]++;
    }
    for (const auto& [slot, ranges] : info.liveRangesBySlot) {
        for (const CaptureLiveRange& range : ranges) {
            for (int offset : range.allCloseOffsets) {
                occurrences[offset]++;
            }
        }
    }
    for (const DecodedInstruction& ins : node.instructions) {
        if (ins.op != Op::CLOSE_UPVALUE) {
            continue;
        }
        int count =
            occurrences.contains(ins.offset) ? occurrences.at(ins.offset) : 0;
        EXPECT_EQ(count, 1)
            << "id=" << info.id << ": CLOSE_UPVALUE at offset " << ins.offset
            << " must be the end of EXACTLY one reported range (or reported "
            << "unreachable exactly once), got " << count;
    }
}

// R4 / round-3 referee decision item 4: checks that every live range is
// internally consistent (non-negative, backed by at least one capturing
// closure, non-overlapping with its slot's other ranges) and matches the
// decoded chunk's own CLOSE_UPVALUE offsets in both directions. Called from
// the shared compile-and-analyze helper below, so every test that builds a
// Compiled gets this for free -- no test can skip it, and a check that only
// ever runs on programs with zero orphans to find proves nothing about the
// defect it exists to catch.
void validateCaptureAnalysis(const DecodedFunction& tree,
                             const CaptureAnalysis& captures) {
    std::map<std::string, const DecodedFunction*> byId;
    collectById(tree, byId);

    for (const auto& [id, info] : captures.functions) {
        const DecodedFunction& node = *byId.at(id);
        // R18: CaptureLiveRange no longer carries an `end` field (it only
        // ever duplicated allCloseOffsets.back(), or a chunk-length sentinel
        // for a closedImplicitly-only range). This is the same bound the
        // removed field's sentinel case gave, recomputed locally for the
        // non-overlap check below.
        int chunkEnd = static_cast<int>(node.function->chunk.size());
        for (const auto& [slot, ranges] : info.liveRangesBySlot) {
            int previousEnd = -1;
            for (const CaptureLiveRange& range : ranges) {
                EXPECT_GE(range.firstCaptureOffset, 0)
                    << "id=" << id << " slot=" << slot;
                for (int offset : range.allCloseOffsets) {
                    EXPECT_GE(offset, range.firstCaptureOffset)
                        << "id=" << id << " slot=" << slot;
                }
                EXPECT_FALSE(range.capturingClosureOffsets.empty())
                    << "id=" << id << " slot=" << slot
                    << ": a live range must have at least one capturing "
                    << "closure, or it would never have opened";
                EXPECT_TRUE(!range.allCloseOffsets.empty() ||
                            range.closedImplicitly)
                    << "id=" << id << " slot=" << slot
                    << ": a range must close somehow -- explicitly, "
                    << "implicitly, or both";
                EXPECT_GE(range.firstCaptureOffset, previousEnd)
                    << "id=" << id << " slot=" << slot
                    << ": live ranges of one slot must not overlap";
                previousEnd = range.allCloseOffsets.empty()
                                  ? chunkEnd
                                  : range.allCloseOffsets.back();
            }
        }
        checkCloseUpvaluesMatchDecodedChunk(node, info);
        checkNoOrphanCloseUpvalues(node, info);
    }
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
// soft-fail case. Validates the result before returning it (see
// validateCaptureAnalysis) -- every caller below gets this for free.
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
    validateCaptureAnalysis(result.tree, result.captures);
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
    EXPECT_LT(vRanges[0].allCloseOffsets.back(), wRanges[0].firstCaptureOffset)
        << "v must close before w is even captured -- the two ranges must "
        << "not overlap or share an end offset";
}

// R3 documentation: `firstCaptureOffset` is a bound, not the declaration
// offset, and it does not dominate a range's own explicit closes.
// V3_loopvar.lox is the standing counter-example: a zero-trip loop runs the
// CLOSE_UPVALUE that ends `i`'s range without ever running the CLOSURE that
// opened it (the declaration, the init clause's `var i = 0`, runs once
// before the loop and is not reflected in either field). See
// capture_analysis.h for what this means for N7. This test pins the exact
// counter-example down as an intentional, tested contract, not a silent gap.
TEST(CaptureAnalysisTest, LoopVarRangeDoesNotDominateItsOwnClose) {
    Compiled c = compileFile(projectRoot() / "notes" / "translation-probes" /
                             "V3_loopvar.lox");
    const DecodedFunction* make = findByName(c.tree, "make");
    ASSERT_NE(make, nullptr);
    const FunctionCaptureInfo& info = infoFor(c.captures, make->id);
    const std::vector<CaptureLiveRange>& ranges = rangesFor(info, 2);
    ASSERT_EQ(ranges.size(), 1U);
    ASSERT_FALSE(ranges[0].allCloseOffsets.empty());
    int lastClose = ranges[0].allCloseOffsets.back();

    // The JUMP_IF_FALSE that skips the whole loop body on a zero-trip run
    // must jump to a target that is > firstCaptureOffset (inside the
    // range) and <= its last explicit close -- i.e. some path reaches the
    // range without ever running the instruction firstCaptureOffset names.
    bool foundSkipIntoRange = false;
    for (const DecodedInstruction& ins : make->instructions) {
        if (ins.op == Op::JUMP_IF_FALSE &&
            ins.offset < ranges[0].firstCaptureOffset &&
            ins.jumpTarget > ranges[0].firstCaptureOffset &&
            ins.jumpTarget <= lastClose) {
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
// of a loop-var capture's own alternate exit points must not confuse
// resolution of either one. Here `t` is captured and closed, in its own
// block, strictly between the `continue` path's close of `s` and the
// `break` path's second close of the same `s` range. The CFG puts `t`'s
// block on neither of `s`'s two paths (see analyzeOneChunk in
// capture_analysis.cpp), so `t`'s unrelated open-then-close never reaches
// the dataflow state either of `s`'s two alternate closes sees.
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

// R9 regression, failure case 1. Native output is 3, 13, 23 (loxpp on
// 642687e): `i` is one shared cell across all three iterations. Before the
// fix, the loop body's own two alternate exits for `s` (continue's close,
// then the fall-through close) left the SECOND close with no open slot of
// its own to match, so it fell through to `i` — `i` was reported closed
// inside the loop (perIteration=true), the V3_loopvar defect this whole node
// exists to catch.
TEST(CaptureAnalysisTest, LoopVarNotClosedEarlyByAlternateExit) {
    Compiled c = compileAndAnalyze(R"(
        fun make() {
            var fns = [nil, nil, nil];
            for (var i = 0; i < 3; i = i + 1) {
                var s = i * 10;
                fun f() { return i + s; }
                fns[i] = f;
                if (i == 1) continue;
            }
            return fns;
        }
    )",
                                   "r9_loop_var_alternate_exit");
    const DecodedFunction* make = findByName(c.tree, "make");
    const DecodedFunction* f = findByName(c.tree, "f");
    ASSERT_NE(make, nullptr);
    ASSERT_NE(f, nullptr);
    const FunctionCaptureInfo& info = infoFor(c.captures, make->id);
    const std::vector<ClosureUpvalue>& wiring =
        infoFor(c.captures, f->id).ownUpvalues;
    ASSERT_EQ(wiring.size(), 2U);
    int iSlot = wiring[0].index;
    int sSlot = wiring[1].index;

    const std::vector<CaptureLiveRange>& iRanges = rangesFor(info, iSlot);
    const std::vector<CaptureLiveRange>& sRanges = rangesFor(info, sSlot);
    ASSERT_EQ(iRanges.size(), 1U);
    ASSERT_EQ(sRanges.size(), 1U);
    EXPECT_FALSE(iRanges[0].perIteration)
        << "i's only real close is after the loop exits — one shared cell "
        << "for the whole loop, matching native output 3, 13, 23";
    EXPECT_TRUE(sRanges[0].perIteration)
        << "s is declared inside the loop body — a fresh cell every "
        << "iteration, continue included";
    EXPECT_EQ(iRanges[0].allCloseOffsets.size(), 1U)
        << "i has exactly one real close, after the loop";
    EXPECT_EQ(sRanges[0].allCloseOffsets.size(), 2U)
        << "s has two real alternate closes -- the continue path and the "
        << "normal fall-through -- per the multi-end model";
}

// R9 regression, failure case 2. Native output is 99: `a` is a function-scope
// capture, closed only implicitly when the frame returns. Before the fix,
// the loop body's own two alternate exits for `s` (break's close, then the
// fall-through close) left the SECOND close with no open slot of its own,
// so it fell through to `a` — `a` was reported closed inside the loop,
// leaving `a = 99` (which runs after the loop) writing past the reported
// end of its own cell.
TEST(CaptureAnalysisTest, OuterCaptureNotClosedEarlyByLoopExit) {
    Compiled c = compileAndAnalyze(R"(
        fun make() {
            var fns = [nil];
            var a = 1;
            fun outerCap() { return a; }
            for (var i = 0; i < 2; i = i + 1) {
                var s = i;
                fun f() { return s; }
                fns[0] = f;
                if (i == 0) break;
            }
            a = 99;
            return outerCap;
        }
    )",
                                   "r9_outer_capture_loop_exit");
    const DecodedFunction* make = findByName(c.tree, "make");
    const DecodedFunction* outerCap = findByName(c.tree, "outerCap");
    const DecodedFunction* f = findByName(c.tree, "f");
    ASSERT_NE(make, nullptr);
    ASSERT_NE(outerCap, nullptr);
    ASSERT_NE(f, nullptr);
    const FunctionCaptureInfo& info = infoFor(c.captures, make->id);
    int aSlot = infoFor(c.captures, outerCap->id).ownUpvalues.at(0).index;
    int sSlot = infoFor(c.captures, f->id).ownUpvalues.at(0).index;
    ASSERT_NE(aSlot, sSlot);

    const std::vector<CaptureLiveRange>& aRanges = rangesFor(info, aSlot);
    const std::vector<CaptureLiveRange>& sRanges = rangesFor(info, sSlot);
    ASSERT_EQ(aRanges.size(), 1U);
    ASSERT_EQ(sRanges.size(), 1U);
    EXPECT_TRUE(aRanges[0].closedImplicitly)
        << "a is function scope — the frame's RETURN closes it, matching "
        << "native output 99: the write at `a = 99` must land before that "
        << "close, not after a wrongly-early one";
    EXPECT_TRUE(aRanges[0].allCloseOffsets.empty())
        << "a has no explicit close on any path -- closedImplicitly alone "
        << "ends it";
    EXPECT_FALSE(sRanges[0].closedImplicitly)
        << "s's own scope (the loop body) closes it with a real "
        << "CLOSE_UPVALUE, on whichever of its two exit paths runs";
    EXPECT_EQ(sRanges[0].allCloseOffsets.size(), 2U)
        << "s has two real alternate closes -- the break path and the "
        << "normal fall-through -- per the multi-end model";
}

// R10 regression. Native output is 42, 42: f and g share one cell. Before
// the fix, `s`'s close on the dead `continue` path (never taken — `k` is
// always 0) was treated as final, so g's later capture of the same slot
// opened a SECOND, separate range instead of joining f's.
TEST(CaptureAnalysisTest, RecapturedSlotAfterDeadEarlyExitSharesOneCell) {
    Compiled c = compileAndAnalyze(R"(
        fun make() {
            var fns = [nil, nil];
            for (var k = 0; k < 1; k = k + 1) {
                var s = 5;
                fun f() { return s; }
                fns[0] = f;
                if (k == 99) continue;
                fun g() { return s; }
                fns[1] = g;
                s = 42;
            }
            return fns;
        }
    )",
                                   "r10_recapture_after_dead_exit");
    const DecodedFunction* make = findByName(c.tree, "make");
    const DecodedFunction* f = findByName(c.tree, "f");
    ASSERT_NE(make, nullptr);
    ASSERT_NE(f, nullptr);
    const FunctionCaptureInfo& info = infoFor(c.captures, make->id);
    int sSlot = infoFor(c.captures, f->id).ownUpvalues.at(0).index;

    const std::vector<CaptureLiveRange>& sRanges = rangesFor(info, sSlot);
    ASSERT_EQ(sRanges.size(), 1U)
        << "f and g capture the SAME s -- one live range, one runtime cell, "
        << "matching native output 42, 42";
    EXPECT_EQ(sRanges[0].capturingClosureOffsets.size(), 2U)
        << "f and g must both be recorded as sharing this one range";
    EXPECT_EQ(sRanges[0].allCloseOffsets.size(), 2U)
        << "s has two real alternate closes -- the dead continue path and "
        << "the normal fall-through -- per the multi-end model";
}

// Runs the pass over one file and checks it never throws. compileFile
// already runs validateCaptureAnalysis on the result (see compileAndAnalyze
// above), so this only needs to name the file for a failing EXPECT_*.
void checkNoAssertionFailure(const fs::path& path) {
    SCOPED_TRACE("file=" + path.string());
    Compiled c = compileFile(path);
    (void)c;
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

// R13 regression. Native output is 0, 1, 2: `a` is declared inside an `if`
// that is itself inside the loop body, and its only CLOSE_UPVALUE (the
// if-block's own endScope) sits before the loop's back-edge, so it needs a
// fresh cell every iteration -- the exact V1_fresh_cell shape, one `if`
// removed from it. Before the CFG rewrite, the pass reopened this slot at
// the enclosing JUMP_IF_FALSE span's exit as though it might still be
// captured by an unrelated later variable, so the range ran to the chunk's
// end (closedImplicitly) and lost its perIteration verdict.
TEST(CaptureAnalysisTest, PerIterationCaptureInsideIfInsideLoop) {
    Compiled c = compileAndAnalyze(R"(
        fun make() {
            var fns = [nil, nil, nil];
            for (var i = 0; i < 3; i = i + 1) {
                if (i >= 0) {
                    var a = i;
                    fun f() { return a; }
                    fns[i] = f;
                }
            }
            return fns;
        }
    )",
                                   "r13_capture_inside_if_inside_loop");
    const DecodedFunction* make = findByName(c.tree, "make");
    const DecodedFunction* f = findByName(c.tree, "f");
    ASSERT_NE(make, nullptr);
    ASSERT_NE(f, nullptr);
    const FunctionCaptureInfo& info = infoFor(c.captures, make->id);
    int aSlot = infoFor(c.captures, f->id).ownUpvalues.at(0).index;

    const std::vector<CaptureLiveRange>& ranges = rangesFor(info, aSlot);
    ASSERT_EQ(ranges.size(), 1U);
    EXPECT_FALSE(ranges[0].closedImplicitly)
        << "the if-block's own endScope closes a with a real CLOSE_UPVALUE "
        << "every time the if-body runs";
    EXPECT_TRUE(ranges[0].perIteration)
        << "a is declared inside the loop body -- a fresh cell every "
        << "iteration, matching native output 0, 1, 2";
    EXPECT_EQ(ranges[0].allCloseOffsets.size(), 1U)
        << "no break/continue here -- a has exactly one real close";
}

// R14 regression. Native output is 1, then 3: `f` captures `a` (an `if`
// block's own local) and `h` captures `d` (a later, unrelated block's own
// local that happens to reuse a's slot number once a's block truly ends).
// Before the CFG rewrite, the pass reopened a's slot at the if's own
// JUMP_IF_FALSE span exit -- even though a's own CLOSE_UPVALUE is not one of
// several alternate exits, it is the ONE real, final end of a's scope -- so
// d's later capture of the same slot number joined a's range instead of
// starting its own, and the pass reported one shared cell for two closures
// that must never share one.
TEST(CaptureAnalysisTest, DifferentVariablesReusingOneSlotDoNotShareACell) {
    Compiled c = compileAndAnalyze(R"(
        fun make(c) {
            var fns = [nil, nil];
            if (c) {
                var a = 1;
                fun f() { return a; }
                fns[0] = f;
            }
            {
                var d = 3;
                fun h() { return d; }
                fns[1] = h;
            }
            return fns;
        }
    )",
                                   "r14_distinct_variables_share_a_slot");
    const DecodedFunction* make = findByName(c.tree, "make");
    const DecodedFunction* f = findByName(c.tree, "f");
    const DecodedFunction* h = findByName(c.tree, "h");
    ASSERT_NE(make, nullptr);
    ASSERT_NE(f, nullptr);
    ASSERT_NE(h, nullptr);
    const FunctionCaptureInfo& info = infoFor(c.captures, make->id);
    int aSlot = infoFor(c.captures, f->id).ownUpvalues.at(0).index;
    int dSlot = infoFor(c.captures, h->id).ownUpvalues.at(0).index;
    ASSERT_EQ(aSlot, dSlot) << "the test must exercise real slot reuse, or "
                            << "it proves nothing about this defect";

    const std::vector<CaptureLiveRange>& ranges = rangesFor(info, aSlot);
    ASSERT_EQ(ranges.size(), 2U)
        << "a and d are different variables -- two live ranges, two cells, "
        << "matching native output 1, 3";
    EXPECT_EQ(ranges[0].capturingClosureOffsets.size(), 1U);
    EXPECT_EQ(ranges[1].capturingClosureOffsets.size(), 1U);
    EXPECT_EQ(ranges[0].allCloseOffsets.size(), 1U)
        << "a's if-block has exactly one real close, its own endScope";
    EXPECT_EQ(ranges[1].allCloseOffsets.size(), 1U)
        << "d's block has exactly one real close, its own endScope";
    EXPECT_LE(ranges[0].allCloseOffsets.back(), ranges[1].firstCaptureOffset)
        << "a must close before d is even captured -- the two ranges must "
        << "not overlap";
}
// R16 regression, failure case 1 (a silent wrong attribution, no crash).
// Native output is 1, then 5: `g` captures `b` and `h` captures both `c`
// and `a` -- ONE CLOSURE instruction with two `isLocal=1` upvalues, opening
// two DIFFERENT ranges at the SAME origin offset. Before the fix,
// rangeIndexByOrigin keyed on that shared offset alone, so the second
// slot's insertion silently overwrote the first's index: `c`'s range (the
// one written first) lost its own index and inherited whichever range `a`
// happened to occupy.
TEST(CaptureAnalysisTest,
     OneClosureCapturingTwoSlotsAttributesEachToItsOwnRange) {
    Compiled c = compileAndAnalyze(R"(
        fun outer() {
          { var b = 1; fun g() { return b; } print g(); }
          { var c = 2; var a = 3; fun h() { return c + a; } print h(); }
        }
        outer();
    )",
                                   "r16_one_closure_two_slots");
    const DecodedFunction* outer = findByName(c.tree, "outer");
    const DecodedFunction* g = findByName(c.tree, "g");
    const DecodedFunction* h = findByName(c.tree, "h");
    ASSERT_NE(outer, nullptr);
    ASSERT_NE(g, nullptr);
    ASSERT_NE(h, nullptr);
    const FunctionCaptureInfo& info = infoFor(c.captures, outer->id);
    int bSlot = infoFor(c.captures, g->id).ownUpvalues.at(0).index;
    const std::vector<ClosureUpvalue>& hWiring =
        infoFor(c.captures, h->id).ownUpvalues;
    ASSERT_EQ(hWiring.size(), 2U)
        << "h must capture both c and a -- one CLOSURE, two isLocal "
        << "upvalues -- or this test proves nothing about the defect";
    int cSlot = hWiring[0].index;
    int aSlot = hWiring[1].index;
    ASSERT_EQ(bSlot, cSlot)
        << "b and c legitimately reuse the same slot number once b's block "
        << "ends -- the test must exercise a REUSED slot, or it does not "
        << "reach the shape R16 exists to catch";
    ASSERT_NE(cSlot, aSlot);

    const std::vector<CaptureLiveRange>& slot1Ranges = rangesFor(info, bSlot);
    const std::vector<CaptureLiveRange>& slot2Ranges = rangesFor(info, aSlot);
    ASSERT_EQ(slot1Ranges.size(), 2U)
        << "b's incarnation and c's incarnation of the reused slot -- two "
        << "ranges, and neither may steal the other's close";
    ASSERT_EQ(slot2Ranges.size(), 1U);
    EXPECT_EQ(slot1Ranges[0].allCloseOffsets.size(), 1U)
        << "b's block has exactly one real close, its own endScope -- not a "
        << "second, false end at h's opening offset";
    EXPECT_EQ(slot1Ranges[1].allCloseOffsets.size(), 1U)
        << "c's incarnation must get its OWN real close, not lose it "
        << "because h's single CLOSURE instruction also opened a's range "
        << "at the identical offset and overwrote c's stored index";
    EXPECT_EQ(slot2Ranges[0].allCloseOffsets.size(), 1U);
    EXPECT_LE(slot1Ranges[0].allCloseOffsets.back(),
              slot1Ranges[1].firstCaptureOffset)
        << "b's incarnation must close before c's begins -- they must not "
        << "overlap";
    EXPECT_EQ(slot1Ranges[1].firstCaptureOffset,
              slot2Ranges[0].firstCaptureOffset)
        << "c and a are opened by the SAME CLOSURE instruction -- the exact "
        << "shape R16 exists to catch";
    EXPECT_NE(slot1Ranges[1].allCloseOffsets.back(),
              slot2Ranges[0].allCloseOffsets.back())
        << "c and a close at DIFFERENT offsets -- one must not silently "
        << "read the other's stale index and report its end instead";
}

// R16 regression, failure case 2 (a heap-buffer-overflow / SIGSEGV before
// the fix, on a fully legal program). A THIRD block whose own CLOSURE also
// captures two slots makes the stale index -- left over from the SECOND
// block's own two-slot CLOSURE overwriting the first slot's entry -- larger
// than the vector it wrongly pointed into, so reading it read past the end
// of that vector.
TEST(CaptureAnalysisTest,
     OneClosureCapturingTwoSlotsAcrossThreeBlocksDoesNotOverflow) {
    Compiled c = compileAndAnalyze(R"(
        fun outer() {
          { var b = 1; fun g() { return b; } print g(); }
          { var c = 2; fun h() { return c; } print h(); }
          { var d = 3; var e = 4; fun k() { return e + d; } print k(); }
        }
        outer();
    )",
                                   "r16_three_blocks_stale_index");
    const DecodedFunction* outer = findByName(c.tree, "outer");
    const DecodedFunction* k = findByName(c.tree, "k");
    ASSERT_NE(outer, nullptr);
    ASSERT_NE(k, nullptr);
    const FunctionCaptureInfo& info = infoFor(c.captures, outer->id);
    const std::vector<ClosureUpvalue>& kWiring =
        infoFor(c.captures, k->id).ownUpvalues;
    ASSERT_EQ(kWiring.size(), 2U);
    int eSlot = kWiring[0].index;
    int dSlot = kWiring[1].index;
    ASSERT_NE(eSlot, dSlot);

    const std::vector<CaptureLiveRange>& eRanges = rangesFor(info, eSlot);
    const std::vector<CaptureLiveRange>& dRanges = rangesFor(info, dSlot);
    ASSERT_FALSE(eRanges.empty());
    ASSERT_FALSE(dRanges.empty());
    EXPECT_EQ(eRanges.back().allCloseOffsets.size(), 1U)
        << "e must get its own real close, at k's own CLOSURE offset -- not "
        << "read out of bounds of a different slot's range vector";
    EXPECT_EQ(dRanges.back().allCloseOffsets.size(), 1U);
    EXPECT_EQ(eRanges.back().firstCaptureOffset,
              dRanges.back().firstCaptureOffset)
        << "e and d are opened by the SAME CLOSURE instruction -- the exact "
        << "shape R16 exists to catch";
    EXPECT_NE(eRanges.back().allCloseOffsets.back(),
              dRanges.back().allCloseOffsets.back())
        << "e and d close at DIFFERENT offsets";
}

// R17 regression, failure case 1 (a silent wrong answer before the fix).
// Native output is 2, then 1: `f` captures `a`, a function-scope capture
// with no close of its own on any path; `g` captures `b`, an if-block local
// whose ONLY capturing CLOSURE runs on the branch that returns immediately,
// while the if-block's own endScope CLOSE_UPVALUE only ever runs on the
// SIBLING (false) path -- the one that never ran that CLOSURE. Before the
// fix, "highest open slot" picked `a` -- the only slot dynamically open on
// that sibling path -- ending `a`'s range early (a live cell a later read
// still needed) and leaving `b`'s range with no end recorded at all.
TEST(CaptureAnalysisTest,
     CaptureOnEarlyReturnBranchClosesOnSiblingFallThrough) {
    Compiled c = compileAndAnalyze(R"(
        fun outer(cond) {
          var a = 1;
          fun f() { return a; }
          {
            var b = 2;
            if (cond) { fun g() { return b; } return g; }
          }
          return f;
        }
        print outer(true)();
        print outer(false)();
    )",
                                   "r17_early_return_sibling_close");
    const DecodedFunction* outer = findByName(c.tree, "outer");
    const DecodedFunction* f = findByName(c.tree, "f");
    const DecodedFunction* g = findByName(c.tree, "g");
    ASSERT_NE(outer, nullptr);
    ASSERT_NE(f, nullptr);
    ASSERT_NE(g, nullptr);
    const FunctionCaptureInfo& info = infoFor(c.captures, outer->id);
    int aSlot = infoFor(c.captures, f->id).ownUpvalues.at(0).index;
    int bSlot = infoFor(c.captures, g->id).ownUpvalues.at(0).index;
    ASSERT_NE(aSlot, bSlot);
    ASSERT_LT(aSlot, bSlot)
        << "a is declared before the if-block and b inside it -- b must get "
        << "the HIGHER slot number, or this test does not reach the shape "
        << "R17 exists to catch (the wrong pick only ever favours a LOWER "
        << "slot that happens to be dynamically open)";

    const std::vector<CaptureLiveRange>& aRanges = rangesFor(info, aSlot);
    const std::vector<CaptureLiveRange>& bRanges = rangesFor(info, bSlot);
    ASSERT_EQ(aRanges.size(), 1U);
    ASSERT_EQ(bRanges.size(), 1U);
    EXPECT_TRUE(aRanges[0].closedImplicitly)
        << "a is function scope -- only the frame's RETURN ends it, on "
        << "every path";
    EXPECT_TRUE(aRanges[0].allCloseOffsets.empty())
        << "a must not pick up the if-block's own endScope close -- that "
        << "belongs to b, not a";
    EXPECT_FALSE(bRanges[0].allCloseOffsets.empty())
        << "b's range must record the if-block's own endScope close, even "
        << "though the branch that captures b never dynamically reaches it";
    EXPECT_TRUE(bRanges[0].closedImplicitly)
        << "the branch that captures b returns before the if-block's own "
        << "endScope runs, so b is ALSO still open at that RETURN, on the "
        << "other path";
}

// R17 regression, failure case 2 (a thrown "no open captured slot to
// close" error before the fix, on a fully legal program). A second,
// unconditional capture (`a`) before the branch puts TWO CLOSE_UPVALUE
// instructions in the common scope exit: one for `b` (the branch-only
// capture) and one for `a` (unconditional). Before the fix, the FIRST of
// the two closes mis-targeted `a` (the only slot dynamically open on the
// sibling path), leaving the state empty by the time the SECOND close ran
// -- which then had no open captured slot left to close at all.
TEST(CaptureAnalysisTest, CaptureOnEarlyReturnBranchClosesInCommonScopeExit) {
    Compiled c = compileAndAnalyze(R"(
        fun outer(cond) {
          var out = [];
          {
            var a = 1;
            var b = 2;
            fun f() { return a; }
            out.append(f);
            if (cond) { fun g() { return b; } return g; }
          }
          return out;
        }
        print outer(true)();
        print outer(false)[0]();
    )",
                                   "r17_early_return_common_scope_exit");
    const DecodedFunction* outer = findByName(c.tree, "outer");
    const DecodedFunction* f = findByName(c.tree, "f");
    const DecodedFunction* g = findByName(c.tree, "g");
    ASSERT_NE(outer, nullptr);
    ASSERT_NE(f, nullptr);
    ASSERT_NE(g, nullptr);
    const FunctionCaptureInfo& info = infoFor(c.captures, outer->id);
    int aSlot = infoFor(c.captures, f->id).ownUpvalues.at(0).index;
    int bSlot = infoFor(c.captures, g->id).ownUpvalues.at(0).index;
    ASSERT_NE(aSlot, bSlot);
    ASSERT_LT(aSlot, bSlot)
        << "a is declared before b -- b must get the HIGHER slot number, "
        << "or this test does not reach the shape R17 exists to catch";

    const std::vector<CaptureLiveRange>& aRanges = rangesFor(info, aSlot);
    const std::vector<CaptureLiveRange>& bRanges = rangesFor(info, bSlot);
    ASSERT_EQ(aRanges.size(), 1U);
    ASSERT_EQ(bRanges.size(), 1U);
    EXPECT_EQ(aRanges[0].allCloseOffsets.size(), 1U)
        << "a's own explicit close, the SECOND CLOSE_UPVALUE of the common "
        << "scope exit -- reaching it must not throw";
    EXPECT_EQ(bRanges[0].allCloseOffsets.size(), 1U)
        << "b's own explicit close, the FIRST CLOSE_UPVALUE of the common "
        << "scope exit -- even though the branch that captures b never "
        << "dynamically reaches it";
    EXPECT_TRUE(aRanges[0].closedImplicitly);
    EXPECT_TRUE(bRanges[0].closedImplicitly);
    EXPECT_LT(bRanges[0].allCloseOffsets.back(),
              aRanges[0].allCloseOffsets.back())
        << "the compiler reclaims locals in descending slot order -- b (the "
        << "higher slot) must close before a (the lower slot) in the SAME "
        << "common scope exit";
}

// Amendment 1's own adversarial shape ("a static close above a live outer
// capture", referee amendment 1 section 7): two SEPARATE if-with-early-
// return blocks, each needing the amended (static-overlay) resolution on its
// own, one after the other, and BOTH reusing the same slot number once the
// first if-block's own scope truly ends (the R14 shape, layered onto R17).
// `keep` is a genuine, live, unconditional outer capture opened BEFORE
// either if-block and never explicitly closed anywhere; its slot number is
// necessarily lower than both if-block locals' (declared later). Before the
// amendment, "highest open slot in the per-path state" would have picked
// `keep` at BOTH if-blocks' own static-only closes -- it is the only slot
// dynamically open on either sibling (false) path -- ending `keep` far too
// early and leaving neither if-block local's own close recorded at all.
TEST(CaptureAnalysisTest, TwoSiblingStaticClosesDoNotStealAnOuterCapturesSlot) {
    Compiled c = compileAndAnalyze(R"(
        fun outer(cond1, cond2) {
          var keep = 1;
          fun keepFn() { return keep; }
          {
            var x = 2;
            if (cond1) { fun gx() { return x; } return gx; }
          }
          {
            var y = 3;
            if (cond2) { fun gy() { return y; } return gy; }
          }
          return keepFn;
        }
        print outer(true, false)();
        print outer(false, true)();
        print outer(false, false)();
    )",
                                   "outer_capture_steal");
    const DecodedFunction* outer = findByName(c.tree, "outer");
    const DecodedFunction* keepFn = findByName(c.tree, "keepFn");
    const DecodedFunction* gx = findByName(c.tree, "gx");
    const DecodedFunction* gy = findByName(c.tree, "gy");
    ASSERT_NE(outer, nullptr);
    ASSERT_NE(keepFn, nullptr);
    ASSERT_NE(gx, nullptr);
    ASSERT_NE(gy, nullptr);
    const FunctionCaptureInfo& info = infoFor(c.captures, outer->id);
    int keepSlot = infoFor(c.captures, keepFn->id).ownUpvalues.at(0).index;
    int xSlot = infoFor(c.captures, gx->id).ownUpvalues.at(0).index;
    int ySlot = infoFor(c.captures, gy->id).ownUpvalues.at(0).index;
    ASSERT_LT(keepSlot, xSlot)
        << "keep is declared first -- it must get the LOWEST slot, or a "
        << "buggy pick that always favours the lowest open slot would not "
        << "be caught here";
    ASSERT_EQ(xSlot, ySlot)
        << "x's block fully ends before y's begins -- the test must "
        << "exercise a REUSED slot, layering R14 onto R17, or it does not "
        << "reach the shape this test exists to catch";

    const std::vector<CaptureLiveRange>& keepRanges = rangesFor(info, keepSlot);
    const std::vector<CaptureLiveRange>& xyRanges = rangesFor(info, xSlot);
    ASSERT_EQ(keepRanges.size(), 1U);
    ASSERT_EQ(xyRanges.size(), 2U)
        << "x's incarnation and y's incarnation of the reused slot -- two "
        << "ranges, neither stolen by the other or by keep";
    EXPECT_TRUE(keepRanges[0].closedImplicitly)
        << "keep is function scope -- only the frame's RETURN ends it";
    EXPECT_TRUE(keepRanges[0].allCloseOffsets.empty())
        << "keep must not pick up EITHER if-block's own endScope close -- "
        << "those belong to x and y, not to keep";
    EXPECT_EQ(xyRanges[0].allCloseOffsets.size(), 1U)
        << "x's if-block has exactly one real close, its own endScope";
    EXPECT_EQ(xyRanges[1].allCloseOffsets.size(), 1U)
        << "y's if-block has exactly one real close, its own endScope";
    EXPECT_TRUE(xyRanges[0].closedImplicitly)
        << "the branch that captures x returns before x's own endScope "
        << "runs, so x is ALSO still open at that RETURN, on the other path";
    EXPECT_TRUE(xyRanges[1].closedImplicitly)
        << "same reasoning as x, for y's own early-return branch";
    EXPECT_LE(xyRanges[0].allCloseOffsets.back(),
              xyRanges[1].firstCaptureOffset)
        << "x must fully close before y is even captured -- the two "
        << "incarnations must not overlap";
}

// R19 regression, failure case 1. Native output is 99: `a` is a
// function-scope capture (closed only implicitly at the frame's own
// RETURN); `b` is captured only inside a nested if-with-early-return, and
// its own scope's endScope close is a STATIC, overlay-only one (R17's
// shape) on the sibling path. Before the fix, advanceProbe blindly popped
// `state`'s own highest slot at every CLOSE_UPVALUE, including that static
// close -- and on the sibling ("cond=false") path, `a` (an unrelated,
// lower, still-open ENCLOSING capture) was the only thing open, so the
// blind pop removed `a` instead of leaving it alone. That corrupted every
// later block's entry state, so `s`'s later re-capture of `a` opened a
// SECOND range instead of joining the first.
TEST(CaptureAnalysisTest,
     StaticCloseDoesNotStealAnEnclosingCaptureAcrossABlockBoundary) {
    Compiled c = compileAndAnalyze(R"(
        fun outer(cond) {
          var a = 1;
          fun g() { return a; }
          {
            var b = 2;
            if (cond) { fun h() { return b; } return h; }
          }
          if (cond) { print "unused"; }
          fun s() { a = 99; }
          s();
          return g;
        }
        print outer(false)();
    )",
                                   "r19_static_close_across_block_boundary");
    const DecodedFunction* outer = findByName(c.tree, "outer");
    const DecodedFunction* g = findByName(c.tree, "g");
    const DecodedFunction* h = findByName(c.tree, "h");
    const DecodedFunction* s = findByName(c.tree, "s");
    ASSERT_NE(outer, nullptr);
    ASSERT_NE(g, nullptr);
    ASSERT_NE(h, nullptr);
    ASSERT_NE(s, nullptr);
    const FunctionCaptureInfo& info = infoFor(c.captures, outer->id);
    int aSlot = infoFor(c.captures, g->id).ownUpvalues.at(0).index;
    int bSlot = infoFor(c.captures, h->id).ownUpvalues.at(0).index;
    int aSlotAgain = infoFor(c.captures, s->id).ownUpvalues.at(0).index;
    ASSERT_EQ(aSlot, aSlotAgain)
        << "s and g must capture the SAME variable, or this test does not "
        << "reach the shape R19 exists to catch";
    ASSERT_NE(aSlot, bSlot);

    const std::vector<CaptureLiveRange>& aRanges = rangesFor(info, aSlot);
    const std::vector<CaptureLiveRange>& bRanges = rangesFor(info, bSlot);
    ASSERT_EQ(aRanges.size(), 1U)
        << "g and s hold the same variable a -- ONE shared cell, or a "
        << "backend that obeys the map prints 1 where native prints 99";
    ASSERT_EQ(bRanges.size(), 1U);
    EXPECT_EQ(aRanges[0].capturingClosureOffsets.size(), 2U)
        << "both g and s must be recorded as sharing a's one range";
    EXPECT_TRUE(aRanges[0].closedImplicitly);
    EXPECT_TRUE(aRanges[0].allCloseOffsets.empty())
        << "a has no explicit close on any path -- b's own static close "
        << "must not steal it";
    EXPECT_EQ(bRanges[0].allCloseOffsets.size(), 1U)
        << "b's own static, overlay-only endScope close";
}

// R19 regression, failure case 2. Native output is 0, then 1: `snap` gets a
// fresh cell each iteration, and its live range ends on whichever of the
// loop body's own two alternate exits (continue, or the normal
// fall-through) runs. Before the fix, advanceProbe's blind pop wrongly
// consumed `snap` at `b`'s own static close (the same root cause as the
// case above), corrupting state so that NEITHER later alternate exit found
// anything open -- the second one reached throws
// "has no open captured slot to close in either layer" on a legal program.
TEST(CaptureAnalysisTest,
     PerIterationCaptureSurvivesAStaticSiblingCloseInsideTheLoop) {
    Compiled c = compileAndAnalyze(R"(
        fun outer(cond) {
          var acc = [];
          for (var i = 0; i < 3; i = i + 1) {
            var snap = i;
            fun mk() { return snap; }
            acc.append(mk);
            {
              var b = 0;
              if (cond) { fun h() { return b; } return h; }
            }
            if (i == 1) { continue; }
            acc.append(1);
          }
          return acc;
        }
        var r = outer(false);
        print r[0]();
        print r[2]();
    )",
                                   "r19_static_close_inside_loop_alternates");
    const DecodedFunction* outer = findByName(c.tree, "outer");
    const DecodedFunction* mk = findByName(c.tree, "mk");
    const DecodedFunction* h = findByName(c.tree, "h");
    ASSERT_NE(outer, nullptr);
    ASSERT_NE(mk, nullptr);
    ASSERT_NE(h, nullptr);
    const FunctionCaptureInfo& info = infoFor(c.captures, outer->id);
    int snapSlot = infoFor(c.captures, mk->id).ownUpvalues.at(0).index;
    int bSlot = infoFor(c.captures, h->id).ownUpvalues.at(0).index;
    ASSERT_NE(snapSlot, bSlot);

    const std::vector<CaptureLiveRange>& snapRanges = rangesFor(info, snapSlot);
    const std::vector<CaptureLiveRange>& bRanges = rangesFor(info, bSlot);
    ASSERT_EQ(snapRanges.size(), 1U);
    ASSERT_EQ(bRanges.size(), 1U);
    EXPECT_TRUE(snapRanges[0].perIteration)
        << "snap is declared inside the loop body -- a fresh cell every "
        << "iteration, matching native output 0, 1";
    EXPECT_EQ(snapRanges[0].allCloseOffsets.size(), 2U)
        << "snap closes on whichever alternate loop exit runs -- continue "
        << "or the normal fall-through";
    EXPECT_TRUE(snapRanges[0].closedImplicitly)
        << "the cond=true path returns before either alternate exit runs, "
        << "leaving this iteration's cell open at that RETURN";
    EXPECT_EQ(bRanges[0].allCloseOffsets.size(), 1U)
        << "b's own static, overlay-only endScope close must still resolve, "
        << "not steal snap's slot, and not leave snap's own closes with "
        << "nothing left to find";
}

// R20 regression. Native output is 1, then 2: an ordinary if/else where
// BOTH arms capture the same outer, never-redeclared local must share one
// cell, not be treated as two conflicting incarnations. Before the fix,
// joinInto threw where the two arms' exit states rejoined, because it saw
// one slot open with two different CLOSURE offsets and assumed that meant
// decoder or compiler drift.
TEST(CaptureAnalysisTest, IfElseArmsCapturingOneOuterVariableShareOneCell) {
    Compiled c = compileAndAnalyze(R"(
        fun outer(c) {
          var x = 1;
          var f = nil;
          if (c) { fun a() { return x; } f = a; }
          else { fun b() { return x + 1; } f = b; }
          return f;
        }
        print outer(true)();
        print outer(false)();
    )",
                                   "r20_if_else_arms_share_one_outer_capture");
    const DecodedFunction* outer = findByName(c.tree, "outer");
    const DecodedFunction* a = findByName(c.tree, "a");
    const DecodedFunction* b = findByName(c.tree, "b");
    ASSERT_NE(outer, nullptr);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    const FunctionCaptureInfo& info = infoFor(c.captures, outer->id);
    int aSlot = infoFor(c.captures, a->id).ownUpvalues.at(0).index;
    int bSlot = infoFor(c.captures, b->id).ownUpvalues.at(0).index;
    ASSERT_EQ(aSlot, bSlot)
        << "a and b must capture the SAME slot, or this test does not "
        << "reach the shape R20 exists to catch";

    const std::vector<CaptureLiveRange>& ranges = rangesFor(info, aSlot);
    ASSERT_EQ(ranges.size(), 1U)
        << "both if/else arms capture the same never-redeclared x -- ONE "
        << "range, or the merge must have thrown or split it";
    EXPECT_EQ(ranges[0].capturingClosureOffsets.size(), 2U)
        << "both a and b must be recorded as sharing this one range";
    EXPECT_TRUE(ranges[0].closedImplicitly)
        << "x is function scope -- only the frame's RETURN ends it";
}

// R20 regression, layered onto a loop: the loop variable itself is
// captured by both if/else arms, on every iteration. Native output is 2,
// then 2 (both closures see the loop var's final value, one shared cell for
// the whole loop).
TEST(CaptureAnalysisTest,
     IfElseArmsInsideALoopCapturingTheLoopVarShareOneCell) {
    Compiled c = compileAndAnalyze(R"(
        fun outer(c) {
          var acc = [];
          for (var i = 0; i < 2; i = i + 1) {
            if (c) { fun a() { return i; } acc.append(a); }
            else { fun b() { return i; } acc.append(b); }
          }
          return acc;
        }
        var r = outer(true);
        print r[0]();
        print r[1]();
    )",
                                   "r20_if_else_arms_in_loop_share_loop_var");
    const DecodedFunction* outer = findByName(c.tree, "outer");
    const DecodedFunction* a = findByName(c.tree, "a");
    const DecodedFunction* b = findByName(c.tree, "b");
    ASSERT_NE(outer, nullptr);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    const FunctionCaptureInfo& info = infoFor(c.captures, outer->id);
    int aSlot = infoFor(c.captures, a->id).ownUpvalues.at(0).index;
    int bSlot = infoFor(c.captures, b->id).ownUpvalues.at(0).index;
    ASSERT_EQ(aSlot, bSlot);

    const std::vector<CaptureLiveRange>& ranges = rangesFor(info, aSlot);
    ASSERT_EQ(ranges.size(), 1U)
        << "one shared cell for the whole loop, matching native output "
        << "2, 2";
    EXPECT_EQ(ranges[0].capturingClosureOffsets.size(), 2U);
    EXPECT_FALSE(ranges[0].perIteration)
        << "the loop var's own close is after the loop exits, not inside "
        << "its back-edge span";
}

// R20 regression, layered onto a loop a second way: a fresh, per-iteration
// body local, captured by both if/else arms on each iteration. Native
// output is 0, then 1 (each iteration's own fresh cell).
TEST(CaptureAnalysisTest,
     IfElseArmsInsideALoopCapturingAPerIterationLocalShareOneCell) {
    Compiled c = compileAndAnalyze(R"(
        fun outer(c) {
          var acc = [];
          for (var i = 0; i < 2; i = i + 1) {
            var snap = i;
            if (c) { fun a() { return snap; } acc.append(a); }
            else { fun b() { return snap; } acc.append(b); }
          }
          return acc;
        }
        var r = outer(true);
        print r[0]();
        print r[1]();
    )",
                                   "r20_if_else_arms_in_loop_share_body_local");
    const DecodedFunction* outer = findByName(c.tree, "outer");
    const DecodedFunction* a = findByName(c.tree, "a");
    const DecodedFunction* b = findByName(c.tree, "b");
    ASSERT_NE(outer, nullptr);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    const FunctionCaptureInfo& info = infoFor(c.captures, outer->id);
    int aSlot = infoFor(c.captures, a->id).ownUpvalues.at(0).index;
    int bSlot = infoFor(c.captures, b->id).ownUpvalues.at(0).index;
    ASSERT_EQ(aSlot, bSlot);

    const std::vector<CaptureLiveRange>& ranges = rangesFor(info, aSlot);
    ASSERT_EQ(ranges.size(), 1U)
        << "one fresh cell PER ITERATION, shared by both arms of that same "
        << "iteration, matching native output 0, 1";
    EXPECT_EQ(ranges[0].capturingClosureOffsets.size(), 2U);
    EXPECT_TRUE(ranges[0].perIteration);
}

// R21 (nit): none of the shapes above capture a slot AGAIN after a static,
// overlay-only close AND a block boundary -- the one shape that reads the
// per-block entry state advanceProbe could have damaged. Native output is
// 1, 3, then 2, 3: whichever if/else arm ran, x is still open afterward,
// and d's later capture must join the SAME merged range, not open a third
// one.
TEST(CaptureAnalysisTest, RecaptureAfterIfElseMergeJoinsTheMergedRange) {
    Compiled c = compileAndAnalyze(R"(
        fun outer(c) {
          var x = 1;
          var f = nil;
          if (c) { fun a() { return x; } f = a; }
          else { fun b() { return x + 1; } f = b; }
          fun d() { return x + 2; }
          return [f, d];
        }
        var r1 = outer(true);
        print r1[0]();
        print r1[1]();
        var r2 = outer(false);
        print r2[0]();
        print r2[1]();
    )",
                                   "r21_recapture_after_if_else_merge");
    const DecodedFunction* outer = findByName(c.tree, "outer");
    const DecodedFunction* a = findByName(c.tree, "a");
    const DecodedFunction* b = findByName(c.tree, "b");
    const DecodedFunction* d = findByName(c.tree, "d");
    ASSERT_NE(outer, nullptr);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(d, nullptr);
    const FunctionCaptureInfo& info = infoFor(c.captures, outer->id);
    int aSlot = infoFor(c.captures, a->id).ownUpvalues.at(0).index;
    int bSlot = infoFor(c.captures, b->id).ownUpvalues.at(0).index;
    int dSlot = infoFor(c.captures, d->id).ownUpvalues.at(0).index;
    ASSERT_EQ(aSlot, bSlot);
    ASSERT_EQ(aSlot, dSlot);

    const std::vector<CaptureLiveRange>& ranges = rangesFor(info, aSlot);
    ASSERT_EQ(ranges.size(), 1U)
        << "a, b, and d all hold the same x -- one range, not a third one "
        << "for d's later, post-merge recapture";
    EXPECT_EQ(ranges[0].capturingClosureOffsets.size(), 3U)
        << "a, b, and d must all be recorded as sharing this one range";
    EXPECT_TRUE(ranges[0].closedImplicitly);
}
