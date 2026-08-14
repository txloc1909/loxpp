// test_backend_abstract_stack.cpp — abstract-stack reconstruction checkpoint
// (notes/backend-implementation-dag.md, node N2).
//
// Checkpoint, verbatim from N2.md:
//   1. 01_assign_local: the POP at offset 8 is TEMP; the POP at offset 12 is
//      LOCAL-RECLAIM. Assert the reason, not only the label.
//   2. 15_nested_arith: the computed maximum stack depth equals an
//      independent hand count.
//   3. The stack height is exactly 0 at every RETURN in every probe, after
//      the return value is accounted for.
//   4. The analysis runs over every probe, every examples/*.lox, and
//      bootstrap/loxpp_interpreter.lox with no inconsistency.
//   5. At every control-flow merge, the stack height from all predecessors
//      is the same. Assert this.
//
// Item 5 is enforced *inside* analyzeStack itself (a merge disagreement
// throws), so any corpus walk below that completes without throwing has
// already exercised it on every merge in that program.

#include "backend/abstract_stack.h"
#include "backend/chunk_decoder.h"
#include "compiler.h"
#include "memory_manager.h"
#include "object.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
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

DecodedFunction decodeSource(const std::string& source, MemoryManager& mm) {
    ObjFunction* script = compile(source, &mm);
    if (script == nullptr) {
        throw std::runtime_error("compilation failed");
    }
    return decodeFunctionTree(script);
}

// Finds a POP's classification by source offset. Fails the assertion (does
// not throw) if no POP exists at that offset, so a caller sees a clear
// message instead of a silent empty result.
PopKind popKindAtOffset(const FunctionStackAnalysis& analysis, int offset) {
    for (const PopClassification& p : analysis.pops) {
        if (p.offset == offset) {
            return p.kind;
        }
    }
    ADD_FAILURE() << "no POP recorded at offset " << offset;
    return PopKind::TEMP;
}

const StackState& stateBeforeOffset(const DecodedFunction& fn,
                                    const FunctionStackAnalysis& analysis,
                                    int offset) {
    for (size_t i = 0; i < fn.instructions.size(); i++) {
        if (fn.instructions[i].offset == offset) {
            return analysis.before[i];
        }
    }
    throw std::runtime_error("no instruction at offset " +
                             std::to_string(offset));
}

// Checkpoint 4/5: analyzeStack runs to completion with no inconsistency
// anywhere in the tree. The merge-consistency half of checkpoint 5 is
// enforced *inside* analyzeStack (a disagreement throws), so a corpus walk
// that completes without throwing has already exercised it on every merge
// in that program. Recurses into every nested function, mirroring
// DecodedFunction's own shape.
void checkNoInconsistency(const DecodedFunction& node,
                          const std::string& path) {
    SCOPED_TRACE("function id=" + node.id + " path=" + path);
    analyzeStack(node);
    for (const DecodedFunction& child : node.nested) {
        checkNoInconsistency(child, path + " > " + child.displayName);
    }
}

// Checkpoint 3, scoped exactly as N2.md states it: "every probe". It does
// not hold program-wide — a `match` expression whose arm is a block of
// fully-discarded statements (no trailing bare expression) can leave the
// arm's "value" sitting in a local slot instead of a temporary (observed in
// bootstrap/loxpp_interpreter.lox's `resolveStmt`); RETURNing that value
// needs an explicit load, which is an emitter (N4) concern, not a P1 defect.
void checkReturnHeightZero(const DecodedFunction& node,
                           const std::string& path) {
    SCOPED_TRACE("function id=" + node.id + " path=" + path);
    FunctionStackAnalysis analysis = analyzeStack(node);
    for (size_t i = 0; i < node.instructions.size(); i++) {
        if (!analysis.reached[i] || node.instructions[i].op != Op::RETURN) {
            continue;
        }
        EXPECT_EQ(analysis.before[i].operandDepth(), 1)
            << "RETURN at offset " << node.instructions[i].offset
            << " does not have exactly the return value on the operand "
            << "stack (height=" << analysis.before[i].height
            << ", localCount=" << analysis.before[i].localCount << ")";
    }
    for (const DecodedFunction& child : node.nested) {
        checkReturnHeightZero(child, path + " > " + child.displayName);
    }
}

void checkFileNoInconsistency(const fs::path& path) {
    SCOPED_TRACE("file=" + path.string());
    MemoryManager mm;
    DecodedFunction tree = decodeSource(readFile(path), mm);
    checkNoInconsistency(tree, path.filename().string());
}

void checkProbeFile(const fs::path& path) {
    SCOPED_TRACE("file=" + path.string());
    MemoryManager mm;
    DecodedFunction tree = decodeSource(readFile(path), mm);
    checkNoInconsistency(tree, path.filename().string());
    checkReturnHeightZero(tree, path.filename().string());
}

} // namespace

// Checkpoint 5, directly: analyzeStack must throw on a genuine merge
// disagreement, not just complete without throwing on programs that happen
// not to have one (R3 — this reviewer finding is why the corpus tests above
// are worded to say the throw is what they are exercising). Bypasses the
// compiler to hand-build a chunk no real compiler would ever emit: two
// paths into one PRINT, one of which pushes an extra CONSTANT the other
// does not, so the merge disagrees by one cell of height with nothing
// recognized as local on either side (not the legitimate differing-arity
// case runFixpoint's comment discusses — that one keeps operand depth
// equal; this one deliberately does not).
TEST(AbstractStackTest, MergeDisagreementThrows) {
    ObjFunction fakeFn; // arity 0; never registered with a MemoryManager —
                        // analyzeStack only reads `arity`, and Obj's
                        // constructor needs no allocator.
    DecodedFunction fn;
    fn.function = &fakeFn;
    fn.id = "0";
    fn.displayName = "malformed";

    DecodedInstruction constant0;
    constant0.offset = 0;
    constant0.op = Op::CONSTANT;

    DecodedInstruction jumpIfFalse;
    jumpIfFalse.offset = 3;
    jumpIfFalse.op = Op::JUMP_IF_FALSE;
    jumpIfFalse.jumpTarget = 9; // skips the extra CONSTANT below

    DecodedInstruction constant1;
    constant1.offset = 6;
    constant1.op = Op::CONSTANT;

    DecodedInstruction print;
    print.offset = 9;
    print.op = Op::PRINT;

    fn.instructions = {constant0, jumpIfFalse, constant1, print};

    // R9: assert the message, not only std::runtime_error's type.
    // analyzeStack throws that type from three places (the unknown-opcode
    // guard, the structural height/localCount guard, and
    // validateMergeConsistency); a plain EXPECT_THROW(..., std::runtime_error)
    // would stay green even if a future change made a *different* guard fire
    // first on this chunk, silently retiring checkpoint 5's own assertion —
    // exactly the failure mode R3 reported for this same test.
    try {
        analyzeStack(fn);
        ADD_FAILURE() << "analyzeStack did not throw; the jump-taken edge "
                      << "reaches PRINT one cell shallower than the "
                      << "fallthrough edge, and analyzeStack must not "
                      << "silently max its way past that";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("merge disagreement"),
                  std::string::npos)
            << "analyzeStack threw, but not from validateMergeConsistency: "
            << e.what();
    }
}

// Checkpoint 1. Quotes the disassembly straight from N2.md, which the
// orchestrator verified against the real `-DLOXPP_DEBUG_PRINT_CODE` output.
TEST(AbstractStackTest, AssignLocalClassifiesBothPopsByReasonNotJustLabel) {
    MemoryManager mm;
    DecodedFunction script =
        decodeSource(readFile(projectRoot() / "notes" / "translation-probes" /
                              "01_assign_local.lox"),
                     mm);
    FunctionStackAnalysis analysis = analyzeStack(script);

    // The reason offset 8 is TEMP: PRINT already consumed its operand, so
    // the cell POP removes here sits *above* the local region.
    const StackState& atOffset8 = stateBeforeOffset(script, analysis, 8);
    EXPECT_GT(atOffset8.operandDepth(), 0)
        << "offset 8's POP should discard a temporary above the local region";
    EXPECT_EQ(popKindAtOffset(analysis, 8), PopKind::TEMP);

    // The reason offset 12 is LOCAL_RECLAIM: the cell POP removes here is
    // exactly the top of the local region (operandDepth 0) — block exit
    // reclaiming `a`, not a leftover expression result.
    const StackState& atOffset12 = stateBeforeOffset(script, analysis, 12);
    EXPECT_EQ(atOffset12.operandDepth(), 0)
        << "offset 12's POP should reclaim the last live local, not a "
        << "temporary";
    EXPECT_EQ(popKindAtOffset(analysis, 12), PopKind::LOCAL_RECLAIM);

    // Slot 1 (`a`) must be recognized as an invisible var: offset 0 pushes
    // its value with no store opcode (P1).
    bool foundSite = false;
    for (const InvisibleVarSite& site : analysis.invisibleVars) {
        if (site.slot == 1 && site.offset == 0) {
            foundSite = true;
        }
    }
    EXPECT_TRUE(foundSite) << "offset 0 (the invisible `var a`) was not "
                           << "recognized as slot 1's declaring push";
}

// Checkpoint 2. `print (1 + 2) * (3 - 4) / 5 - -6;` — hand count below.
//
// Starting height is 1 (slot 0 = the script's own closure; VM::interpret
// calls it via the ordinary zero-arg `call()` path, same as any function).
//   CONSTANT 1        height 1->2
//   CONSTANT 2        height 2->3
//   ADD               height 3->2   (running max so far: 3)
//   CONSTANT 3        height 2->3
//   CONSTANT 4        height 3->4   <- high-water mark: 4
//   SUBTRACT          height 4->3
//   MULTIPLY          height 3->2
//   CONSTANT 5        height 2->3
//   DIVIDE            height 3->2
//   CONSTANT 6        height 2->3
//   NEGATE            height 3->3
//   SUBTRACT          height 3->2
//   PRINT             height 2->1
// Raw high-water mark = 4. Excluding the 1 bottom slot (localCount=1, never
// bumped — nothing here is a named local): operand high-water mark = 3.
TEST(AbstractStackTest, NestedArithMaxStackMatchesHandCount) {
    MemoryManager mm;
    DecodedFunction script =
        decodeSource(readFile(projectRoot() / "notes" / "translation-probes" /
                              "15_nested_arith.lox"),
                     mm);
    FunctionStackAnalysis analysis = analyzeStack(script);
    EXPECT_EQ(analysis.maxOperandDepth, 3);
}

// Checkpoint 2, a second probe (R2): 15_nested_arith declares no local, so it
// cannot tell a correct maxOperandDepth from one inflated by counting a
// not-yet-recognized local as an operand cell. `find_leaf`
// (examples/at_binding_demo.lox) declares several: a wrong recognition-timing
// implementation reports 4 here; correct is 3 (hand-verified against a
// per-offset dump — every local's declaring push is recognized at the push
// itself, so none of it is ever double-counted as a temporary).
TEST(AbstractStackTest, MaxStackWithLocalsIsNotInflatedByLateRecognition) {
    MemoryManager mm;
    DecodedFunction script = decodeSource(
        readFile(projectRoot() / "examples" / "at_binding_demo.lox"), mm);
    ASSERT_FALSE(script.nested.empty());
    const DecodedFunction* findLeaf = nullptr;
    for (const DecodedFunction& fn : script.nested) {
        if (fn.displayName == "find_leaf") {
            findLeaf = &fn;
        }
    }
    ASSERT_NE(findLeaf, nullptr)
        << "find_leaf not found among nested functions";
    FunctionStackAnalysis analysis = analyzeStack(*findLeaf);
    EXPECT_EQ(analysis.maxOperandDepth, 3);
}

// Checkpoint 3 + 4 + 5 for the probe corpus specifically.
TEST(AbstractStackTest, RunsOverEveryProbeWithNoInconsistency) {
    std::vector<fs::path> probes =
        listLoxFiles(projectRoot() / "notes" / "translation-probes");
    ASSERT_FALSE(probes.empty()) << "no translation probes found";
    for (const fs::path& probe : probes) {
        checkProbeFile(probe);
    }
}

// Checkpoint 4 + 5 over the wider corpus (checkpoint 3 is probe-scoped only
// — see checkReturnHeightZero's comment).
TEST(AbstractStackTest, RunsOverEveryExampleWithNoInconsistency) {
    std::vector<fs::path> examples = listLoxFiles(projectRoot() / "examples");
    ASSERT_FALSE(examples.empty()) << "no example programs found";
    for (const fs::path& example : examples) {
        checkFileNoInconsistency(example);
    }
}

TEST(AbstractStackTest, RunsOverBootstrapInterpreterWithNoInconsistency) {
    checkFileNoInconsistency(projectRoot() / "bootstrap" /
                             "loxpp_interpreter.lox");
}

// V1 is the sharpest probe for capture lifetimes (bytecode-translation-
// problems.md P4): `snapshot` is captured *inside* the loop body, so its
// declaring push and CLOSE_UPVALUE must both resolve to real slots, not be
// silently dropped as unrecognized locals.
TEST(AbstractStackTest, FreshCellProbeHasNoUnexplainedTemporaries) {
    MemoryManager mm;
    DecodedFunction script =
        decodeSource(readFile(projectRoot() / "notes" / "translation-probes" /
                              "V1_fresh_cell.lox"),
                     mm);
    ASSERT_FALSE(script.nested.empty());
    // `make`, the function that declares and captures `snapshot` each
    // iteration.
    FunctionStackAnalysis analysis = analyzeStack(script.nested[0]);
    EXPECT_GT(analysis.invisibleVars.size(), 0U);
}

// R7: findDeclaringPushes must follow a slot's *value*, not its stack
// position. 11_for_in.lox is the sharpest probe for this: BUILD_LIST pops
// three cells and pushes the list back at position 1, then GET_ITER
// replaces it in place — a wrong, position-following implementation walks
// past both and blames the first CONSTANT (offset 0) for slot 1, instead of
// BUILD_LIST (offset 9), where the list value that actually becomes the
// iterator local is born. Asserts the *entire* list, not one site, so a
// regression can't hide behind an untested second entry the way the old
// single-site assertion in AssignLocalClassifiesBothPopsByReasonNotJustLabel
// did for this exact bug.
TEST(AbstractStackTest, ForInInvisibleVarsNameTheirOwnDeclaringPush) {
    MemoryManager mm;
    DecodedFunction script =
        decodeSource(readFile(projectRoot() / "notes" / "translation-probes" /
                              "11_for_in.lox"),
                     mm);
    FunctionStackAnalysis analysis = analyzeStack(script);

    std::vector<std::pair<int, int>> sites;
    for (const InvisibleVarSite& site : analysis.invisibleVars) {
        sites.emplace_back(site.offset, site.slot);
    }
    std::sort(sites.begin(), sites.end());

    // slot 1 (the hidden iterator): BUILD_LIST's own push lands there, not
    // the CONSTANT at offset 0 that BUILD_LIST later consumes and replaces.
    // slot 2 (the loop variable `x`): NIL's push, unaffected by this bug —
    // included so a regression that shifts *either* site trips this test.
    const std::vector<std::pair<int, int>> expected = {{9, 1}, {12, 2}};
    EXPECT_EQ(sites, expected);
}

// R8: a local that no instruction ever reads back must still get its own
// invisible-var site. `a` here is never read; only its sibling `b` is. A
// wrong implementation finds a site for `b` alone, and recognizing `b`'s
// slot then jumps `localCount` past `a`'s cell with no site to explain it —
// `a` silently becomes "local" with nothing marking where its store must go.
// Asserts the complete invisibleVars list and the per-offset operand depth
// at every offset, not only the final one, so a regression cannot hide
// behind a state that happens to self-correct by the last instruction (see
// this test's own comment on the second, non-corpus symptom below).
TEST(AbstractStackTest, UnreadSiblingLocalGetsItsOwnDeclaringPushSite) {
    MemoryManager mm;
    DecodedFunction script = decodeSource(R"(
{
  var a = 1;
  var b = 2;
  print b;
}
)",
                                          mm);
    FunctionStackAnalysis analysis = analyzeStack(script);

    std::vector<std::pair<int, int>> sites;
    for (const InvisibleVarSite& site : analysis.invisibleVars) {
        sites.emplace_back(site.offset, site.slot);
    }
    std::sort(sites.begin(), sites.end());
    const std::vector<std::pair<int, int>> expected = {{0, 1}, {3, 2}};
    EXPECT_EQ(sites, expected)
        << "slot 1 (`a`, never read back) must get its own site at its own "
        << "declaring push (offset 0), not be silently swept in by slot 2's "
        << "(`b`'s) recognition at offset 3";

    // The full per-offset trace (verified against the real disassembly):
    //   0: CONSTANT '1'   before d=0 after d=0   <- invisible `a`, slot 1
    //   3: CONSTANT '2'   before d=0 after d=0   <- invisible `b`, slot 2
    //   6: GET_LOCAL 2    before d=0 after d=1   <- loads `b`, a real temp
    //   8: PRINT          before d=1 after d=0   <- consumes that temp
    //   9: POP            before d=0 after d=0   <- reclaims `b` (slot 2)
    //  10: POP            before d=0 after d=0   <- reclaims `a` (slot 1)
    //  11: NIL            before d=0 after d=1   <- implicit return value
    //  12: RETURN         before d=1 after d=0
    // Depth is never inflated to 2: if `a` were still misclassified as a
    // temporary, offset 3's `after` would read d=1, not d=0 (R8's exact
    // symptom — see the reviewer's own trace of this bug).
    const std::vector<std::pair<int, int>> expectedDepths = {
        {0, 0}, {0, 0}, {0, 1}, {1, 0}, {0, 0}, {0, 0}, {0, 1}, {1, 0}};
    ASSERT_EQ(script.instructions.size(), expectedDepths.size());
    for (size_t i = 0; i < script.instructions.size(); i++) {
        SCOPED_TRACE("offset=" + std::to_string(script.instructions[i].offset));
        EXPECT_EQ(analysis.before[i].operandDepth(), expectedDepths[i].first);
        EXPECT_EQ(analysis.after[i].operandDepth(), expectedDepths[i].second);
    }
}

// Orchestrator round-3 guidance: a local's frame can end with *no* explicit
// reclaim opcode at all. `never` here is unread (like `a` above) *and* it is
// never popped anywhere in the chunk — a function's own top-level scope has
// no enclosing block left to close, so the whole frame is simply discarded
// at RETURN. Nothing in the reference-driven search, and no sibling POP to
// backfill from either: only RETURN's own operandDepth()==1 invariant
// (checkpoint 3) proves every slot below it is local. A wrong implementation
// leaves `never`'s cell an unrecognized temporary forever, inflating
// maxOperandDepth and misclassifying any later POP that would have reclaimed
// it — the same silent failure as R8, in a shape backfillUnreadSiblingSites
// alone cannot reach (there is no sibling site to backfill *from*).
TEST(AbstractStackTest, LocalWithNoExplicitReclaimIsStillFoundViaReturn) {
    MemoryManager mm;
    DecodedFunction script = decodeSource(R"(
fun f() {
  var a = 1;
  var never = 99;
  print a;
}
f();
)",
                                          mm);
    ASSERT_FALSE(script.nested.empty());
    const DecodedFunction* f = nullptr;
    for (const DecodedFunction& fn : script.nested) {
        if (fn.displayName == "f") {
            f = &fn;
        }
    }
    ASSERT_NE(f, nullptr) << "f not found among nested functions";
    FunctionStackAnalysis analysis = analyzeStack(*f);

    std::vector<std::pair<int, int>> sites;
    for (const InvisibleVarSite& site : analysis.invisibleVars) {
        sites.emplace_back(site.offset, site.slot);
    }
    std::sort(sites.begin(), sites.end());
    const std::vector<std::pair<int, int>> expected = {{0, 1}, {3, 2}};
    EXPECT_EQ(sites, expected)
        << "slot 2 (`never`, unread and never explicitly popped) must still "
        << "get a site, found by chasing down from RETURN's own operand "
        << "depth of 1, not from any sibling reference or POP";

    // Full trace: 0 CONSTANT '1' (a); 3 CONSTANT '99' (never); 6 GET_LOCAL 1
    // (a); 8 PRINT; 9 NIL; 10 RETURN. Depth is 0 everywhere except the two
    // genuine temporaries (the printed copy of `a`, and the return value).
    const std::vector<std::pair<int, int>> expectedDepths = {
        {0, 0}, {0, 0}, {0, 1}, {1, 0}, {0, 1}, {1, 0}};
    ASSERT_EQ(f->instructions.size(), expectedDepths.size());
    for (size_t i = 0; i < f->instructions.size(); i++) {
        SCOPED_TRACE("offset=" + std::to_string(f->instructions[i].offset));
        EXPECT_EQ(analysis.before[i].operandDepth(), expectedDepths[i].first);
        EXPECT_EQ(analysis.after[i].operandDepth(), expectedDepths[i].second);
    }
}

// Orchestrator round-3 guidance proposed deriving localCount from "a maximal
// run of adjacent POP and CLOSE_UPVALUE is one scope-exit group, [whose]
// size is exactly the number of locals that were live before it." This probe
// is the counter-example: the expression-statement `a + 2;` discards an
// unrelated *temporary* with a plain POP, and that POP sits immediately
// next to the block's own single-local reclaim POP for `a`, with nothing
// between them. A byte-adjacency-only "maximal run" reads this as size 2 and
// concludes 2 locals were live, misidentifying the ADD's result as a second
// declared local. This analysis does not use adjacency to size a reclaim
// run at all — see backfillFromFrameTeardown's and
// backfillUnreadSiblingSites's own comments — so it must still tell these
// two, byte-identical POPs apart correctly.
TEST(AbstractStackTest, AdjacentTempPopAndReclaimPopAreNotConflated) {
    MemoryManager mm;
    DecodedFunction script = decodeSource(R"(
{
  var a = 1;
  a + 2;
}
)",
                                          mm);
    FunctionStackAnalysis analysis = analyzeStack(script);

    std::vector<std::pair<int, int>> sites;
    for (const InvisibleVarSite& site : analysis.invisibleVars) {
        sites.emplace_back(site.offset, site.slot);
    }
    std::sort(sites.begin(), sites.end());
    EXPECT_EQ(sites, (std::vector<std::pair<int, int>>{{0, 1}}))
        << "only `a` is a local; the ADD result must not be misidentified "
        << "as a second one";

    // 9: POP discards a+2's unread result (TEMP). 10: POP reclaims `a`
    // (LOCAL_RECLAIM). Byte-identical opcodes, adjacent, opposite meaning —
    // P1 again, one instruction later.
    EXPECT_EQ(popKindAtOffset(analysis, 9), PopKind::TEMP);
    EXPECT_EQ(popKindAtOffset(analysis, 10), PopKind::LOCAL_RECLAIM);

    const std::vector<std::pair<int, int>> expectedDepths = {
        {0, 0}, {0, 1}, {1, 2}, {2, 1}, {1, 0}, {0, 0}, {0, 1}, {1, 0}};
    ASSERT_EQ(script.instructions.size(), expectedDepths.size());
    for (size_t i = 0; i < script.instructions.size(); i++) {
        SCOPED_TRACE("offset=" + std::to_string(script.instructions[i].offset));
        EXPECT_EQ(analysis.before[i].operandDepth(), expectedDepths[i].first);
        EXPECT_EQ(analysis.after[i].operandDepth(), expectedDepths[i].second);
    }
}

TEST(AbstractStackTest, PeeksInsteadOfPopsMatchesTheDocumentedFamily) {
    EXPECT_TRUE(peeksInsteadOfPops(Op::SET_LOCAL));
    EXPECT_TRUE(peeksInsteadOfPops(Op::SET_GLOBAL));
    EXPECT_TRUE(peeksInsteadOfPops(Op::SET_UPVALUE));
    EXPECT_TRUE(peeksInsteadOfPops(Op::SET_PROPERTY));
    EXPECT_TRUE(peeksInsteadOfPops(Op::SET_INDEX));
    EXPECT_TRUE(peeksInsteadOfPops(Op::JUMP_IF_FALSE));
    EXPECT_FALSE(peeksInsteadOfPops(Op::POP));
    EXPECT_FALSE(peeksInsteadOfPops(Op::DEFINE_METHOD));
}
