// test_backend_cfg.cpp — leaders-algorithm CFG recovery tests.
//
// The checkpoint (notes/backend-implementation-dag.md, node N1):
//   1. 05_for.lox's top-level chunk has exactly 2 back edges and 1
//      unconditional forward skip, and its leaders are exactly {0, 3, 12, 16,
//      28, 34}.
//   2. Every jump/loop/jump-table target is a block leader.
//   3. No target lands in the middle of an instruction.
//   4. The leaders algorithm runs over every probe, every examples/*.lox, and
//      bootstrap/loxpp_interpreter.lox with no assertion failure.

#include "backend/cfg.h"
#include "backend/chunk_decoder.h"
#include "compiler.h"
#include "exec_objects.h"
#include "memory_manager.h"
#include "object.h"
#include "value.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

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

const BasicBlock& blockAt(const Cfg& cfg, int leaderOffset) {
    for (const BasicBlock& block : cfg.blocks) {
        if (block.leaderOffset == leaderOffset) {
            return block;
        }
    }
    throw std::runtime_error("no block leads at offset " +
                             std::to_string(leaderOffset));
}

bool hasSuccessor(const BasicBlock& from, const Cfg& cfg, int targetOffset,
                  EdgeKind kind) {
    for (const CfgEdge& edge : from.successors) {
        if (cfg.blocks[static_cast<size_t>(edge.targetBlock)].leaderOffset ==
                targetOffset &&
            edge.kind == kind) {
            return true;
        }
    }
    return false;
}

// Every invariant a CFG must hold for *any* well-formed chunk, independent of
// what that chunk actually computes. Used to sweep the whole corpus (R4 of
// the checkpoint).
void checkInvariants(const Cfg& cfg,
                     const std::vector<DecodedInstruction>& instructions,
                     const std::string& trace) {
    SCOPED_TRACE(trace);
    if (instructions.empty()) {
        EXPECT_TRUE(cfg.blocks.empty());
        return;
    }

    int chunkEnd = instructions.back().offset + instructions.back().length;

    // Blocks are sorted by leaderOffset and tile [0, chunkEnd) with no gap
    // and no overlap.
    ASSERT_FALSE(cfg.blocks.empty());
    EXPECT_EQ(cfg.blocks.front().leaderOffset, 0);
    EXPECT_EQ(cfg.blocks.back().endOffset, chunkEnd);
    for (size_t i = 0; i + 1 < cfg.blocks.size(); i++) {
        EXPECT_EQ(cfg.blocks[i].endOffset, cfg.blocks[i + 1].leaderOffset)
            << "gap or overlap after block " << cfg.blocks[i].label;
        EXPECT_LT(cfg.blocks[i].leaderOffset, cfg.blocks[i].endOffset)
            << "empty block " << cfg.blocks[i].label;
    }

    // Every instruction belongs to exactly one block, and blocks preserve
    // byte order.
    size_t totalInstructions = 0;
    for (const BasicBlock& block : cfg.blocks) {
        totalInstructions += block.instructions.size();
        ASSERT_FALSE(block.instructions.empty());
        EXPECT_EQ(block.instructions.front().offset, block.leaderOffset);
    }
    EXPECT_EQ(totalInstructions, instructions.size());

    // Rule 2: every jump/loop/jump-table target is some block's leader.
    std::set<int> leaderOffsets;
    for (const BasicBlock& block : cfg.blocks) {
        leaderOffsets.insert(block.leaderOffset);
    }
    for (const DecodedInstruction& ins : instructions) {
        switch (ins.op) {
        case Op::JUMP:
        case Op::JUMP_IF_FALSE:
        case Op::LOOP:
            EXPECT_TRUE(leaderOffsets.count(ins.jumpTarget))
                << "target " << ins.jumpTarget << " of the branch at "
                << ins.offset << " is not a block leader";
            break;
        case Op::JUMP_TABLE:
            for (const JumpTableArm& arm : ins.jumpTable) {
                EXPECT_TRUE(leaderOffsets.count(arm.target))
                    << "jump-table arm target " << arm.target
                    << " is not a block leader";
            }
            break;
        default:
            break;
        }
    }

    // RETURN/MATCH_ERROR end a block with no successor; everything else that
    // ends a block has at least one.
    for (const BasicBlock& block : cfg.blocks) {
        Op lastOp = block.instructions.back().op;
        if (lastOp == Op::RETURN || lastOp == Op::MATCH_ERROR) {
            EXPECT_TRUE(block.successors.empty())
                << block.label << " ends in " << static_cast<int>(lastOp)
                << " but has a successor";
        } else {
            EXPECT_FALSE(block.successors.empty())
                << block.label << " has no successor";
        }
    }

    // Predecessors are exactly the transpose of successors.
    for (size_t u = 0; u < cfg.blocks.size(); u++) {
        for (const CfgEdge& edge : cfg.blocks[u].successors) {
            const std::vector<int>& preds =
                cfg.blocks[static_cast<size_t>(edge.targetBlock)].predecessors;
            EXPECT_NE(
                std::find(preds.begin(), preds.end(), static_cast<int>(u)),
                preds.end())
                << cfg.blocks[static_cast<size_t>(edge.targetBlock)].label
                << " is missing predecessor " << cfg.blocks[u].label;
        }
    }

    // Labels are deterministic and unique.
    std::set<std::string> labels;
    for (const BasicBlock& block : cfg.blocks) {
        EXPECT_TRUE(labels.insert(block.label).second)
            << "duplicate label " << block.label;
    }
}

void checkNode(const DecodedFunction& node, const std::string& path) {
    Cfg cfg = buildCfg(node.instructions);
    checkInvariants(cfg, node.instructions,
                    "function id=" + node.id + " path=" + path);
    for (const DecodedFunction& child : node.nested) {
        checkNode(child, path + " > " + child.displayName);
    }
}

void checkSource(const std::string& source, const std::string& label) {
    MemoryManager mm;
    ObjFunction* script = compile(source, &mm);
    if (script == nullptr) {
        throw std::runtime_error("compilation failed for " + label);
    }
    checkNode(decodeFunctionTree(script), label);
}

void checkFile(const fs::path& path) {
    SCOPED_TRACE("file=" + path.string());
    checkSource(readFile(path), path.filename().string());
}

} // namespace

TEST(BackendCfgTest, ForLoopHasExpectedLeadersAndEdges) {
    MemoryManager mm;
    ObjFunction* script =
        compile("for (var i = 0; i < 3; i = i + 1) print i;", &mm);
    ASSERT_NE(script, nullptr);

    DecodedFunction top = decodeFunctionTree(script);
    Cfg cfg = buildCfg(top.instructions);

    // The exact leader set the node spec derives from the real disassembly.
    std::set<int> expectedLeaders = {0, 3, 12, 16, 28, 34};
    std::set<int> actualLeaders;
    for (const BasicBlock& block : cfg.blocks) {
        actualLeaders.insert(block.leaderOffset);
    }
    EXPECT_EQ(actualLeaders, expectedLeaders);

    // JUMP_IF_FALSE 9 -> 34: the loop-exit edge, forward, not the "skip".
    EXPECT_TRUE(
        hasSuccessor(blockAt(cfg, 3), cfg, 34, EdgeKind::FORWARD_BRANCH));
    // Its untaken path falls through into the "skip the increment" block.
    EXPECT_TRUE(hasSuccessor(blockAt(cfg, 3), cfg, 12, EdgeKind::FALL_THROUGH));

    // JUMP 13 -> 28: the one unconditional forward skip.
    EXPECT_TRUE(
        hasSuccessor(blockAt(cfg, 12), cfg, 28, EdgeKind::FORWARD_BRANCH));
    EXPECT_EQ(blockAt(cfg, 12).successors.size(), 1U)
        << "the skip block must have no other successor";

    // LOOP 25 -> 3: back edge, increment to condition.
    EXPECT_TRUE(hasSuccessor(blockAt(cfg, 16), cfg, 3, EdgeKind::BACK_EDGE));
    EXPECT_EQ(blockAt(cfg, 16).successors.size(), 1U);

    // LOOP 31 -> 16: back edge, body to increment.
    EXPECT_TRUE(hasSuccessor(blockAt(cfg, 28), cfg, 16, EdgeKind::BACK_EDGE));
    EXPECT_EQ(blockAt(cfg, 28).successors.size(), 1U);

    // Tally by edge kind across the whole chunk: exactly 2 back edges, and —
    // counting only the *unconditional* forward jump, not JUMP_IF_FALSE's
    // taken path — exactly 1 forward skip.
    int backEdges = 0;
    int unconditionalForwardSkips = 0;
    for (const BasicBlock& block : cfg.blocks) {
        Op lastOp = block.instructions.back().op;
        for (const CfgEdge& edge : block.successors) {
            if (edge.kind == EdgeKind::BACK_EDGE) {
                backEdges++;
            }
            if (edge.kind == EdgeKind::FORWARD_BRANCH && lastOp == Op::JUMP) {
                unconditionalForwardSkips++;
            }
        }
    }
    EXPECT_EQ(backEdges, 2);
    EXPECT_EQ(unconditionalForwardSkips, 1);

    // Labels are derived from the offset, deterministically.
    EXPECT_EQ(blockAt(cfg, 3).label, "L_0003");
    EXPECT_EQ(blockAt(cfg, 34).label, "L_0034");
}

TEST(BackendCfgTest, RejectsTargetLandingMidInstruction) {
    // A hand-built, deliberately corrupt instruction list: the JUMP at
    // offset 0 targets offset 2, which is the second byte of the CONSTANT at
    // offset 1 (length 3) — not the start of any instruction.
    std::vector<DecodedInstruction> instructions;
    DecodedInstruction jump;
    jump.offset = 0;
    jump.op = Op::JUMP;
    jump.length = 3;
    jump.jumpTarget = 2;
    instructions.push_back(jump);

    DecodedInstruction constant;
    constant.offset = 3;
    constant.op = Op::CONSTANT;
    constant.length = 3;
    constant.constantIndex = 0;
    instructions.push_back(constant);

    DecodedInstruction ret;
    ret.offset = 6;
    ret.op = Op::RETURN;
    ret.length = 1;
    instructions.push_back(ret);

    EXPECT_THROW(buildCfg(instructions), std::runtime_error);
}

TEST(BackendCfgTest, RejectsLoopThatPointsForward) {
    // LOOP must always point backward (P3a). A LOOP that points forward
    // means the compiler drifted from the documented contract.
    std::vector<DecodedInstruction> instructions;
    DecodedInstruction loop;
    loop.offset = 0;
    loop.op = Op::LOOP;
    loop.length = 3;
    loop.jumpTarget = 3; // forward, which LOOP must never be
    instructions.push_back(loop);

    DecodedInstruction ret;
    ret.offset = 3;
    ret.op = Op::RETURN;
    ret.length = 1;
    instructions.push_back(ret);

    EXPECT_THROW(buildCfg(instructions), std::runtime_error);
}

TEST(BackendCfgTest, MatchErrorEndsBlockWithNoFallThroughSuccessor) {
    // MATCH_ERROR never returns (vm.cpp); the instruction after it is not a
    // fall-through successor, even though it is a leader (it is also a
    // JUMP_TABLE arm target here, which is why it exists at all).
    MemoryManager mm;
    ObjFunction* script = compile(R"(
        enum Color { Red Green Blue }
        var c = Green;
        var n = match c {
          case Red => 0
          case Green => 1
          case Blue => 2
        };
        print n;
    )",
                                  &mm);
    ASSERT_NE(script, nullptr);
    DecodedFunction top = decodeFunctionTree(script);
    Cfg cfg = buildCfg(top.instructions);

    bool foundMatchError = false;
    for (const BasicBlock& block : cfg.blocks) {
        if (block.instructions.back().op == Op::MATCH_ERROR) {
            foundMatchError = true;
            EXPECT_TRUE(block.successors.empty());
        }
    }
    EXPECT_TRUE(foundMatchError) << "probe did not exercise MATCH_ERROR";
}

TEST(BackendCfgTest, HoldsOverTranslationProbes) {
    std::vector<fs::path> probes =
        listLoxFiles(projectRoot() / "test" / "translation-probes");
    ASSERT_FALSE(probes.empty()) << "no translation probes found";
    for (const fs::path& probe : probes) {
        checkFile(probe);
    }
}

TEST(BackendCfgTest, HoldsOverExamples) {
    std::vector<fs::path> examples = listLoxFiles(projectRoot() / "examples");
    ASSERT_FALSE(examples.empty()) << "no example programs found";
    for (const fs::path& example : examples) {
        checkFile(example);
    }
}

TEST(BackendCfgTest, HoldsOverBootstrapInterpreter) {
    checkFile(projectRoot() / "bootstrap" / "loxpp_interpreter.lox");
}
