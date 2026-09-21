#pragma once

// Handler-depth reconstruction. The compiler tracks open try/catch regions
// in m_openHandlerCount (compiler.h); the VM tracks them in m_handlerStack
// (vm.h). This pass recomputes the expected depth at every instruction of
// one function's own chunk, from the decoded bytes alone.
//
// Target-independent: no VM knowledge beyond PUSH_HANDLER (+1) and
// POP_HANDLER (-1). Both the native unittest suite and any backend can
// consume the result.

#include "chunk_decoder.h"

#include <string>
#include <vector>

// Per-instruction handler depth, aligned 1:1 with the source
// DecodedFunction::instructions.
struct HandlerDepthAnalysis {
    std::string functionId;

    // Depth immediately before / after the instruction runs.
    std::vector<int> before;
    std::vector<int> after;

    // False for an instruction no path from function entry reaches.
    // before/after are meaningless there.
    std::vector<bool> reached;
};

// Analyzes one function's own chunk. Does not recurse into nested
// functions; each has its own frame and starts at depth 0.
//
// Throws std::runtime_error when two incoming edges disagree on depth, when
// a POP_HANDLER drives the depth below zero, or when a POP_HANDLER has no
// matching PUSH_HANDLER. A disagreement means the compiler emitted an
// unbalanced cleanup on some path; the input program is trusted to be
// compiler-correct.
HandlerDepthAnalysis analyzeHandlerDepth(const DecodedFunction& fn);

// Same analysis over a hand-built instruction list, for tests that bypass
// Chunk and the decoder. Offsets only appear in error messages.
HandlerDepthAnalysis
analyzeHandlerDepthIns(const std::vector<DecodedInstruction>& ins,
                       const std::string& functionId = "test");
