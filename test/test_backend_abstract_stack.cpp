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

// Recursively finds a nested function by its display name (a class method or
// a nested `fun`), mirroring DecodedFunction's tree shape. Returns nullptr if
// no such function exists anywhere under `root`.
const DecodedFunction* findFunctionByName(const DecodedFunction& root,
                                          const std::string& name) {
    for (const DecodedFunction& child : root.nested) {
        if (child.displayName == name) {
            return &child;
        }
        if (const DecodedFunction* found = findFunctionByName(child, name)) {
            return found;
        }
    }
    return nullptr;
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
        if (!static_cast<bool>(analysis.reached[i]) ||
            node.instructions[i].op != Op::RETURN) {
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

// R15: validateNoInvisibleVarGaps is a safety net for a bug in discovery
// (findInvisibleVarIndices), not a property any real chunk can trigger —
// analyzeStack's own discovery never reports a slot that disagrees with the
// local count at its own recognition point (that is what R8's redesign
// establishes). So this drives the guard directly, with a `declaredSlotsAt`
// built by hand to disagree on purpose, instead of hunting for a chunk that
// cannot exist.
TEST(AbstractStackTest, DirectlyBuiltGapThrowsWithTheRightMessage) {
    DecodedInstruction constant0;
    constant0.offset = 0;
    constant0.op = Op::CONSTANT; // pops 0, so localCountAfterPops(before, 0)
                                 // is just before.localCount, unchanged: 0.
    std::vector<DecodedInstruction> ins{constant0};
    std::vector<StackState> before{StackState{1, 0}};
    std::vector<bool> reached{true};
    // Claims slot 5 was recognized here, though the local count this
    // instruction's own pops leave behind is 0 — a gap no real discovery
    // result can produce.
    std::vector<std::vector<int>> declaredSlotsAt{{5}};

    try {
        validateNoInvisibleVarGaps(ins, before, reached, declaredSlotsAt, "0");
        ADD_FAILURE() << "validateNoInvisibleVarGaps did not throw on a "
                      << "declaredSlotsAt entry that disagrees with the "
                      << "local count at its own instruction";
    } catch (const std::runtime_error& e) {
        std::string what(e.what());
        EXPECT_NE(what.find("invisible-var recognition gap"), std::string::npos)
            << "wrong exception message: " << what;
        EXPECT_NE(what.find("slot 5"), std::string::npos)
            << "message must name the offending slot: " << what;
        EXPECT_NE(what.find("local count 0"), std::string::npos)
            << "message must name the local count it disagrees with: " << what;
    }
}

// Checkpoint 1. Quotes the disassembly straight from N2.md, which the
// orchestrator verified against the real `-DLOXPP_DEBUG_PRINT_CODE` output.
TEST(AbstractStackTest, AssignLocalClassifiesBothPopsByReasonNotJustLabel) {
    MemoryManager mm;
    DecodedFunction script =
        decodeSource(readFile(projectRoot() / "test" / "translation-probes" /
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
        decodeSource(readFile(projectRoot() / "test" / "translation-probes" /
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
        listLoxFiles(projectRoot() / "test" / "translation-probes");
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
        decodeSource(readFile(projectRoot() / "test" / "translation-probes" /
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
        decodeSource(readFile(projectRoot() / "test" / "translation-probes" /
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
// it — the same silent failure as R8, in a shape the persistence test at
// every POP (findPersistentPopLocals) alone cannot reach either: `never` has
// no POP anywhere in the chunk to run that test at.
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
// run at all — the persistence test at every POP (findPersistentPopLocals)
// decides each POP on its own cover-witness evidence, not on its
// neighbours — so it must still tell these two, byte-identical POPs apart
// correctly.
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

// R8 referee ruling, failure case 1 (N2.md section 5): `{ var a = 1; print
// 2; }` is the sharpest counter-example to the old reference-driven search —
// `a` is unread, and the only other slot in scope (the temporary `print 2`
// pushes) never reaches a POP, so nothing near it looks like a sibling to
// backfill from. The referee's prototype gave site (0,1), POP LOCAL_RECLAIM,
// maxOperandDepth 1; the old code at 0a9ec4f gave none of the three.
TEST(AbstractStackTest, ReferereFailureCase1AssignThenUnrelatedPrint) {
    MemoryManager mm;
    DecodedFunction script = decodeSource("{ var a = 1; print 2; }", mm);
    FunctionStackAnalysis analysis = analyzeStack(script);

    std::vector<std::pair<int, int>> sites;
    for (const InvisibleVarSite& site : analysis.invisibleVars) {
        sites.emplace_back(site.offset, site.slot);
    }
    std::sort(sites.begin(), sites.end());
    EXPECT_EQ(sites, (std::vector<std::pair<int, int>>{{0, 1}}))
        << "`a`'s declaring push must be recognized with no sibling to "
        << "backfill from";

    bool foundReclaim = false;
    for (const PopClassification& p : analysis.pops) {
        EXPECT_EQ(p.kind, PopKind::LOCAL_RECLAIM)
            << "the only POP in this chunk reclaims `a` at block exit; "
            << "`print 2`'s operand is consumed by PRINT, not POP";
        foundReclaim = true;
    }
    EXPECT_TRUE(foundReclaim)
        << "expected exactly one POP (the block's own " << "scope exit)";
    EXPECT_EQ(analysis.maxOperandDepth, 1);
}

// R8 referee ruling: the or-pattern `@`-binding case from the ruling's own
// evidence (N2.md section 5, "unwrap_or"). `v` is never read in either arm —
// only `x` is — so `v`'s two sites (one per or-pattern alternative, P2's
// merge) depend entirely on the persistence test finding a cover witness on
// *each* alternative's own path.
TEST(AbstractStackTest, OrPatternAtBindingUnreadInBothArmsStillGetsBothSites) {
    MemoryManager mm;
    DecodedFunction script = decodeSource(
        readFile(projectRoot() / "examples" / "at_binding_demo.lox"), mm);
    const DecodedFunction* unwrapOr = findFunctionByName(script, "unwrap_or");
    ASSERT_NE(unwrapOr, nullptr)
        << "unwrap_or not found among nested functions";
    FunctionStackAnalysis analysis = analyzeStack(*unwrapOr);

    std::vector<std::pair<int, int>> vSites;
    for (const InvisibleVarSite& site : analysis.invisibleVars) {
        if (site.slot == 6) {
            vSites.emplace_back(site.offset, site.slot);
        }
    }
    std::sort(vSites.begin(), vSites.end());
    EXPECT_EQ(vSites.size(), 2U)
        << "`v` is bound on both or-pattern alternatives; each needs its own "
        << "declaring-push site, and both must survive the persistence test "
        << "even though `v` itself is never read back";
}

// R8 referee ruling: a multi-binding match arm where every binding is
// unread, using bootstrap/loxpp_interpreter.lox's `resolveExpr` — the
// referee's own third failure case (N2.md section 5). Structural, not
// hand-counted offsets: the tail theorem (N2.md section 4) guarantees a
// maximal POP/CLOSE_UPVALUE run reads all-TEMP-then-all-LOCAL_RECLAIM, so a
// run with two or more leading TEMP labels is exactly the corpus invariant
// the reviewer's detector checked (32 -> 0 over the real corpus). This
// asserts that invariant holds for `resolveExpr` specifically, the function
// the referee measured it on.
TEST(AbstractStackTest, ResolveExprHasNoRunWithTwoOrMoreLeadingTempLabels) {
    MemoryManager mm;
    DecodedFunction script = decodeSource(
        readFile(projectRoot() / "bootstrap" / "loxpp_interpreter.lox"), mm);
    const DecodedFunction* resolveExpr =
        findFunctionByName(script, "resolveExpr");
    ASSERT_NE(resolveExpr, nullptr)
        << "resolveExpr not found among nested functions";
    FunctionStackAnalysis analysis = analyzeStack(*resolveExpr);

    std::unordered_map<int, PopKind> kindByOffset;
    for (const PopClassification& p : analysis.pops) {
        kindByOffset[p.offset] = p.kind;
    }

    // The tail theorem needs adjacency by *instruction*, not by position in
    // the filtered `analysis.pops` list: two POPs that are the closest two
    // by offset are only the same scope-exit run if nothing reached lies
    // between them but POP/CLOSE_UPVALUE. resolveExpr has many independent,
    // single-POP expression-statement arms (each its own trivial run of
    // size 1) sitting close together in the match's arm list; treating
    // "next entry in analysis.pops" as "same run" wrongly merges unrelated
    // runs and reports a leading-TEMP streak that was never one run at all.
    // Walk `instructions` itself instead: a run is a maximal span of
    // reached POP/CLOSE_UPVALUE instructions, and it ends the moment any
    // other reached instruction (or the end of the function) appears.
    // CLOSE_UPVALUE is always a reclaim (vm.cpp), never a TEMP.
    size_t tempRun = 0;
    bool sawReclaimThisRun = false;
    bool inRun = false;
    auto endRun = [&](int reportOffset) {
        if (inRun) {
            EXPECT_LT(tempRun, 2U)
                << "the run ending at/before offset " << reportOffset << " has "
                << tempRun << " leading TEMP labels — at least "
                << "one local reclaim was misclassified as a temporary "
                << "(R8's exact symptom)";
        }
        tempRun = 0;
        sawReclaimThisRun = false;
        inRun = false;
    };
    for (size_t idx = 0; idx < resolveExpr->instructions.size(); idx++) {
        if (!static_cast<bool>(analysis.reached[idx])) {
            continue;
        }
        const DecodedInstruction& ins = resolveExpr->instructions[idx];
        if (ins.op != Op::POP && ins.op != Op::CLOSE_UPVALUE) {
            endRun(ins.offset);
            continue;
        }
        inRun = true;
        bool isTemp =
            ins.op == Op::POP && kindByOffset.at(ins.offset) == PopKind::TEMP;
        if (isTemp) {
            EXPECT_FALSE(sawReclaimThisRun)
                << "TEMP at offset " << ins.offset << " follows a "
                << "LOCAL_RECLAIM within the same run — the tail theorem "
                << "(N2.md section 4) requires TEMP labels to come first";
            tempRun++;
        } else {
            sawReclaimThisRun = true;
        }
    }
    endRun(-1);
}

// R8 referee ruling exception: DEFINE_METHOD's own peek disqualifies a cell
// from ever being recognized as a local via a cover witness, even when a
// CLOSURE pushed on top of it looks exactly like one. `classDeclaration`
// (compiler.cpp) pushes the class *twice* here: once as the real local `C`
// (recognized correctly — a block-scoped class is declared like a nested
// `fun`), and again via `namedVariable(className, false)` right after, as a
// throwaway copy that only exists so DEFINE_METHOD can peek(1) at it. That
// throwaway copy's own scope-exit POP is the one the exception must protect:
// without it, the CLOSURE pushed before the *second* method's DEFINE_METHOD
// would wrongly cover it. `C` itself is a separate slot and must still be
// recognized, so this asserts both pops by their distinct reasons rather
// than treating "the only POP" as a single case.
TEST(AbstractStackTest, ClassValueStaysTempAcrossMultipleMethodDefinitions) {
    MemoryManager mm;
    DecodedFunction script = decodeSource(R"(
{
  class C {
    m1() { return 1; }
    m2() { return 2; }
  }
}
)",
                                          mm);
    FunctionStackAnalysis analysis = analyzeStack(script);

    ASSERT_EQ(analysis.invisibleVars.size(), 1U)
        << "exactly one site: `C` itself, recognized at its own CLASS "
        << "instruction; the DEFINE_METHOD-peeked throwaway copy must not "
        << "get one";
    int classSlot = analysis.invisibleVars[0].slot;

    ASSERT_EQ(analysis.pops.size(), 2U)
        << "one POP for the throwaway peek copy, one for the block's own "
        << "scope exit over `C`";
    EXPECT_EQ(analysis.pops[0].kind, PopKind::TEMP)
        << "the first POP reclaims the throwaway copy `namedVariable` pushed "
        << "for DEFINE_METHOD's peek — it must stay TEMP despite the "
        << "CLOSURE pushed on top of it before the second method";
    EXPECT_EQ(analysis.pops[1].kind, PopKind::LOCAL_RECLAIM)
        << "the second POP is the block's real scope exit over `C` itself";

    const StackState& beforeSecondPop =
        stateBeforeOffset(script, analysis, analysis.pops[1].offset);
    EXPECT_EQ(beforeSecondPop.localCount - 1, classSlot)
        << "`C` must be the top local right before its own reclaim";
}

// R12 referee ruling (N2.md section 7): the depth term
// `after.operandDepth() + declaredSlotsAt[i].size()`. Checkpoint 3
// guarantees operand depth 1 immediately before every *reached* RETURN, so
// any function with a reachable RETURN already masks the undercount this
// term fixes — the referee's own measurement found the term changes no
// reported number over the real corpus. To exercise it for real, this
// function's only statement after its two invisible vars is `for (;;) {}`:
// an omitted condition emits no exit test at all (Compiler::forStatement),
// so the loop has no successor and the compiler's own trailing NIL;RETURN
// is unreachable. `a` and `fun g(){}` are both declared, and both
// reclaimed by ordinary POPs at the block's own (reachable) scope exit,
// so — unlike a single lone declaration — neither is the undecidable
// TEMP corner: `g`'s CLOSURE is itself an unconditional declaring push
// (recognized with no cover-witness test needed at all), which doubles as
// `a`'s cover witness, so both sites exist and nothing here reaches depth 1
// except the term under test. The test recomputes both the pre-fix
// ("naive") and the ruled ("corrected") formula from public state alone, so
// it does not depend on hand-picked offsets: it would fail if the
// `+ declaredSlotsAt[i].size()` term were ever reverted.
TEST(AbstractStackTest, MaxOperandDepthCountsAnInvisibleVarsOwnDeclaringPush) {
    MemoryManager mm;
    DecodedFunction script = decodeSource(R"(
fun f() {
  { var a = 1; fun g() {} }
  for (;;) {}
}
f();
)",
                                          mm);
    const DecodedFunction* f = findFunctionByName(script, "f");
    ASSERT_NE(f, nullptr) << "f not found among nested functions";
    FunctionStackAnalysis analysis = analyzeStack(*f);

    std::unordered_map<int, int> newSlotsAtOffset;
    for (const InvisibleVarSite& site : analysis.invisibleVars) {
        newSlotsAtOffset[site.offset]++;
    }

    int naiveMax = 0;
    int correctedMax = 0;
    for (size_t i = 0; i < f->instructions.size(); i++) {
        if (!static_cast<bool>(analysis.reached[i])) {
            continue;
        }
        int offset = f->instructions[i].offset;
        int newSlots = newSlotsAtOffset.count(offset) != 0
                           ? newSlotsAtOffset.at(offset)
                           : 0;
        int naive = std::max(analysis.before[i].operandDepth(),
                             analysis.after[i].operandDepth());
        int corrected = std::max(analysis.before[i].operandDepth(),
                                 analysis.after[i].operandDepth() + newSlots);
        naiveMax = std::max(naiveMax, naive);
        correctedMax = std::max(correctedMax, corrected);
    }

    ASSERT_GT(correctedMax, naiveMax)
        << "this program was chosen so the compiler's trailing NIL;RETURN "
        << "is unreachable (an omitted for-condition has no exit edge) and "
        << "cannot mask the undercount the R12 term fixes; if this fails, "
        << "the chosen program no longer isolates the term";
    EXPECT_EQ(analysis.maxOperandDepth, correctedMax)
        << "analyzeStack must report the corrected bound, not the naive one";
}

// Found while building the R12 test above, not something the referee ruling
// anticipated: a plain `var` at a function's own top-level scope, declared
// *before* a nested `fun`, with the function itself never reaching RETURN
// (an infinite `for (;;) {}` with no exit edge). The persistence test alone
// cannot find such a `var` — there is no POP (it is not inside a block) and
// no reachable RETURN to backfill from (backfillFromFrameTeardown never
// runs). Only the downward chase from the nested `fun`'s own (unconditional)
// CLOSURE site — sound because clox's scoping is strictly LIFO, so a
// confirmed local proves every lower slot is local too — finds it. Without
// that chase, `analyzeStack` throws `validateNoInvisibleVarGaps`'s gap error
// instead of silently miscompiling; still a real regression, since the
// pre-referee-ruling implementation handled this shape correctly.
TEST(AbstractStackTest,
     UnreadLocalBelowANestedFunDeclIsFoundEvenWhenReturnIsUnreachable) {
    MemoryManager mm;
    DecodedFunction script = decodeSource(R"(
fun f() {
  var a = 1;
  fun g() {}
  for (;;) {}
}
f();
)",
                                          mm);
    const DecodedFunction* f = findFunctionByName(script, "f");
    ASSERT_NE(f, nullptr) << "f not found among nested functions";
    FunctionStackAnalysis analysis; // NOLINT(misc-const-correctness)
    ASSERT_NO_THROW(analysis = analyzeStack(*f));

    std::vector<std::pair<int, int>> sites;
    for (const InvisibleVarSite& site : analysis.invisibleVars) {
        sites.emplace_back(site.offset, site.slot);
    }
    std::sort(sites.begin(), sites.end());
    EXPECT_EQ(sites.size(), 2U)
        << "both `a` (slot 1) and `g` (slot 2) must be found; `a` has "
        << "neither a POP nor a reachable RETURN, only the downward chase "
        << "from `g`'s own declaring push";
}

// R14: the test above needs only one downward chase step (`g` sits directly
// above `a`), so it cannot tell `chaseSlotsDownward` apart from a version
// that checks only its own starting slot. This program puts two unread
// locals (`a`, `b`) below the nested `fun`'s own slot, so finding both
// needs the descent to actually walk two slots down, not one.
TEST(AbstractStackTest,
     ChaseFindsEveryUnreadLocalBelowTheAnchorNotJustTheNearestOne) {
    MemoryManager mm;
    DecodedFunction script = decodeSource(R"(
fun f() {
  var a = 1;
  var b = 2;
  fun g() {}
  for (;;) {}
}
f();
)",
                                          mm);
    const DecodedFunction* f = findFunctionByName(script, "f");
    ASSERT_NE(f, nullptr) << "f not found among nested functions";
    FunctionStackAnalysis analysis; // NOLINT(misc-const-correctness)
    ASSERT_NO_THROW(analysis = analyzeStack(*f));

    std::vector<std::pair<int, int>> sites;
    for (const InvisibleVarSite& site : analysis.invisibleVars) {
        sites.emplace_back(site.offset, site.slot);
    }
    std::sort(sites.begin(), sites.end());
    EXPECT_EQ(sites.size(), 3U)
        << "`a` (slot 1), `b` (slot 2), and `g` (slot 3) must all be found; "
        << "`a` is two slots below `g`'s own declaring push, so a chase "
        << "that only checks its own starting slot finds `b` and misses "
        << "`a`";
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
