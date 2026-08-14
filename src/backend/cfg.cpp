#include "backend/cfg.h"

#include <array>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace {

// A branch either transfers control elsewhere (JUMP, JUMP_IF_FALSE, LOOP,
// JUMP_TABLE) or ends the chunk's control flow outright (RETURN,
// MATCH_ERROR — vm.cpp: the latter never returns). Either way, the block it
// sits in ends there, and whatever instruction follows — if any — starts a
// new one, even when nothing else ever jumps to it (rule 3 of the leaders
// algorithm does not care about reachability).
bool isBranch(Op op) {
    switch (op) {
    case Op::JUMP:
    case Op::JUMP_IF_FALSE:
    case Op::LOOP:
    case Op::JUMP_TABLE:
    case Op::RETURN:
    case Op::MATCH_ERROR:
        return true;
    default:
        return false;
    }
}

std::string makeLabel(int offset) {
    std::array<char, 32> buf{};
    std::snprintf(buf.data(), buf.size(), "L_%04d", offset);
    return {buf.data()};
}

// Maps every instruction's start offset to its index in `instructions`, so
// jump targets can be checked against real instruction boundaries in O(1).
std::unordered_map<int, int>
indexInstructionsByOffset(const std::vector<DecodedInstruction>& instructions) {
    std::unordered_map<int, int> byOffset;
    byOffset.reserve(instructions.size());
    for (int i = 0; i < static_cast<int>(instructions.size()); i++) {
        byOffset[instructions[i].offset] = i;
    }
    return byOffset;
}

void requireInstructionBoundary(const std::unordered_map<int, int>& byOffset,
                                int target, int fromOffset,
                                const char* opName) {
    if (byOffset.find(target) == byOffset.end()) {
        throw std::runtime_error("cfg: " + std::string(opName) + " at offset " +
                                 std::to_string(fromOffset) + " targets " +
                                 std::to_string(target) +
                                 ", which is not the start of any instruction");
    }
}

// P3a: JUMP/JUMP_IF_FALSE/JUMP_TABLE arms are always forward, LOOP is always
// backward. A violation means the compiler emitted something this pass does
// not model, not that the direction check itself is wrong.
void requireDirection(int target, int fromOffset, bool wantForward,
                      const char* opName) {
    bool isForward = target > fromOffset;
    if (isForward != wantForward) {
        throw std::runtime_error("cfg: " + std::string(opName) + " at offset " +
                                 std::to_string(fromOffset) + " targets " +
                                 std::to_string(target) + ", which is not " +
                                 (wantForward ? "forward" : "backward") +
                                 " as P3a requires");
    }
}

// Rules 1-3 of the leaders algorithm (Dragon book 2e, 8.4), collected as a
// sorted, deduplicated set of byte offsets.
std::set<int>
collectLeaderOffsets(const std::vector<DecodedInstruction>& instructions,
                     const std::unordered_map<int, int>& byOffset,
                     int chunkEnd) {
    std::set<int> leaders;
    leaders.insert(instructions.front().offset); // rule 1

    for (const DecodedInstruction& ins : instructions) {
        switch (ins.op) {
        case Op::JUMP:
        case Op::JUMP_IF_FALSE:
            requireInstructionBoundary(byOffset, ins.jumpTarget, ins.offset,
                                       "JUMP/JUMP_IF_FALSE");
            requireDirection(ins.jumpTarget, ins.offset, /*wantForward=*/true,
                             "JUMP/JUMP_IF_FALSE");
            leaders.insert(ins.jumpTarget); // rule 2
            break;
        case Op::LOOP:
            requireInstructionBoundary(byOffset, ins.jumpTarget, ins.offset,
                                       "LOOP");
            requireDirection(ins.jumpTarget, ins.offset, /*wantForward=*/false,
                             "LOOP");
            leaders.insert(ins.jumpTarget); // rule 2
            break;
        case Op::JUMP_TABLE:
            for (const JumpTableArm& arm : ins.jumpTable) {
                requireInstructionBoundary(byOffset, arm.target, ins.offset,
                                           "JUMP_TABLE arm");
                requireDirection(arm.target, ins.offset, /*wantForward=*/true,
                                 "JUMP_TABLE arm");
                leaders.insert(arm.target); // rule 2
            }
            break;
        default:
            break;
        }

        if (isBranch(ins.op)) {
            int afterBranch = ins.offset + ins.length;
            if (afterBranch < chunkEnd) {
                leaders.insert(afterBranch); // rule 3
            }
        }
    }

    return leaders;
}

void addEdge(BasicBlock& block, int targetBlock, EdgeKind kind) {
    block.successors.push_back(CfgEdge{targetBlock, kind});
}

// Wires `block`'s successors from its last instruction. Every target here
// was already validated as an instruction boundary by collectLeaderOffsets,
// and every such boundary became a block leader, so `blockIndexOfOffset.at`
// below cannot miss.
void wireSuccessors(BasicBlock& block,
                    const std::unordered_map<int, int>& blockIndexOfOffset,
                    int chunkEnd) {
    const DecodedInstruction& last = block.instructions.back();
    switch (last.op) {
    case Op::JUMP:
        addEdge(block, blockIndexOfOffset.at(last.jumpTarget),
                EdgeKind::FORWARD_BRANCH);
        break;
    case Op::LOOP:
        addEdge(block, blockIndexOfOffset.at(last.jumpTarget),
                EdgeKind::BACK_EDGE);
        break;
    case Op::JUMP_IF_FALSE:
        addEdge(block, blockIndexOfOffset.at(last.jumpTarget),
                EdgeKind::FORWARD_BRANCH);
        if (block.endOffset < chunkEnd) {
            addEdge(block, blockIndexOfOffset.at(block.endOffset),
                    EdgeKind::FALL_THROUGH);
        }
        break;
    case Op::JUMP_TABLE:
        for (const JumpTableArm& arm : last.jumpTable) {
            addEdge(block, blockIndexOfOffset.at(arm.target),
                    EdgeKind::FORWARD_BRANCH);
        }
        if (block.endOffset < chunkEnd) {
            addEdge(block, blockIndexOfOffset.at(block.endOffset),
                    EdgeKind::FALL_THROUGH);
        }
        break;
    case Op::RETURN:
    case Op::MATCH_ERROR:
        break; // no successor
    default:
        if (block.endOffset < chunkEnd) {
            addEdge(block, blockIndexOfOffset.at(block.endOffset),
                    EdgeKind::FALL_THROUGH);
        }
        break;
    }
}

} // namespace

Cfg buildCfg(const std::vector<DecodedInstruction>& instructions) {
    Cfg cfg;
    if (instructions.empty()) {
        return cfg;
    }

    std::unordered_map<int, int> byOffset =
        indexInstructionsByOffset(instructions);
    int chunkEnd = instructions.back().offset + instructions.back().length;

    std::set<int> leaderOffsets =
        collectLeaderOffsets(instructions, byOffset, chunkEnd);

    // Partition instructions at every leader offset. Each leader offset is
    // guaranteed (above) to be a real instruction start, so slicing by
    // instruction index, not by re-scanning bytes, is safe.
    std::vector<int> orderedLeaders(leaderOffsets.begin(), leaderOffsets.end());
    cfg.blocks.reserve(orderedLeaders.size());
    for (size_t b = 0; b < orderedLeaders.size(); b++) {
        int start = orderedLeaders[b];
        int end =
            (b + 1 < orderedLeaders.size()) ? orderedLeaders[b + 1] : chunkEnd;

        BasicBlock block;
        block.leaderOffset = start;
        block.endOffset = end;
        block.label = makeLabel(start);

        int idx = byOffset.at(start);
        while (idx < static_cast<int>(instructions.size()) &&
               instructions[static_cast<size_t>(idx)].offset < end) {
            block.instructions.push_back(
                instructions[static_cast<size_t>(idx)]);
            idx++;
        }

        cfg.blocks.push_back(std::move(block));
    }

    std::unordered_map<int, int> blockIndexOfOffset;
    blockIndexOfOffset.reserve(cfg.blocks.size());
    for (size_t b = 0; b < cfg.blocks.size(); b++) {
        blockIndexOfOffset[cfg.blocks[b].leaderOffset] = static_cast<int>(b);
    }

    for (BasicBlock& block : cfg.blocks) {
        wireSuccessors(block, blockIndexOfOffset, chunkEnd);
    }

    // Predecessors are the transpose of the successor edges just built.
    for (size_t b = 0; b < cfg.blocks.size(); b++) {
        for (const CfgEdge& edge : cfg.blocks[b].successors) {
            cfg.blocks[static_cast<size_t>(edge.targetBlock)]
                .predecessors.push_back(static_cast<int>(b));
        }
    }

    return cfg;
}
