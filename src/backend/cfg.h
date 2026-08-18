#pragma once

// Target-independent control-flow graph over one decoded chunk
// (chunk_decoder.h). Recovers basic blocks with the leaders algorithm
// (Dragon book 2e, section 8.4) and classifies every edge. It carries no
// JVM or CLR knowledge — both backends consume it unchanged.
//
// This is a structural pass only. It does not reconstruct the operand stack
// (abstract_stack.h) or capture upvalues (capture_analysis.h); those passes
// consume the blocks built here.

#include "backend/chunk_decoder.h"

#include <cstdint>
#include <string>
#include <vector>

// How an edge between two blocks arose.
enum class EdgeKind : std::uint8_t {
    // Sequential execution: the block's last instruction does not divert
    // control here. Comes from a non-branching instruction's natural
    // successor, the untaken path of JUMP_IF_FALSE, or the no-match path of
    // JUMP_TABLE.
    FALL_THROUGH,
    // An explicit JUMP, the taken path of JUMP_IF_FALSE, or one arm of
    // JUMP_TABLE. Lox++'s compiler only ever emits these forward (P3a).
    FORWARD_BRANCH,
    // A LOOP. Lox++'s compiler only ever emits LOOP backward (P3a).
    BACK_EDGE,
};

// One outgoing edge, naming the block it leads to by index into Cfg::blocks.
struct CfgEdge {
    int targetBlock{-1};
    EdgeKind kind{EdgeKind::FALL_THROUGH};
};

// A maximal run of instructions with one entry point (the leader) and no
// control transfer except at its last instruction.
struct BasicBlock {
    int leaderOffset{0};
    // One past the last instruction's last byte: the next block's
    // leaderOffset, or the chunk's size for the last block.
    int endOffset{0};

    // Stable name derived from leaderOffset, e.g. "L_0042". Two CFGs built
    // from the same chunk always agree on it.
    std::string label;

    // This block's instructions, in byte order.
    std::vector<DecodedInstruction> instructions;

    std::vector<CfgEdge> successors;
    // Indices into Cfg::blocks of every block with an edge into this one.
    std::vector<int> predecessors;
};

// The basic blocks of one chunk, in byte order.
struct Cfg {
    std::vector<BasicBlock> blocks;
};

// Recovers the CFG of one chunk from its already-decoded instructions (see
// chunk_decoder.h). `instructions` must be exactly what decodeChunk produced
// for that chunk — in byte order, covering the chunk with no gap.
//
// Throws std::runtime_error if a JUMP, JUMP_IF_FALSE, LOOP, or JUMP_TABLE arm
// targets a byte that is not the start of some instruction in `instructions`,
// or if LOOP/JUMP/JUMP_IF_FALSE/JUMP_TABLE point the wrong direction (P3a
// documents LOOP as always backward, the rest as always forward) — either
// means the decoder and this pass have drifted apart, or the compiler emitted
// something this pass does not yet model.
Cfg buildCfg(const std::vector<DecodedInstruction>& instructions);
