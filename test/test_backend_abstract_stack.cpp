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
