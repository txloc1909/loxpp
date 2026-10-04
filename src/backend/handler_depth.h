#pragma once

// Handler-depth reconstruction. The compiler tracks open try/catch regions
// in m_openHandlerCount (compiler.h); the VM tracks them in m_handlerStack
// (vm.h). This pass recomputes the expected depth at every instruction of
// one function's own chunk, from the decoded bytes alone.
//
// Target-independent: no VM knowledge beyond PUSH_HANDLER (+1) and
// POP_HANDLER (-1). Both the native unittest suite and any backend can
// consume the result.
//
// YIELD contract (coroutines mission, tracking #523). The opcode landed with
// the primitive in node #526; this pass's entry is still node #528. YIELD
// (see chunk_decoder.h) has no effect on handler depth. It is a
// fall-through, so before and after are equal and the innermost active handler
// is unchanged: a YIELD inside a protected region leaves that record live
// across a suspension. This pass is compile-time only; preserving the record
// at run time is the VM's job.

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

    // Index (into the SAME instruction list) of the innermost PUSH_HANDLER
    // whose protected region contains this instruction, aligned with
    // `before` (the state immediately BEFORE the instruction runs); -1 when
    // no handler is open. Threaded through the SAME CFG-driven worklist as
    // `before`/`after`, not a plain byte-order bracket scan: a catch
    // block's own bytes sit BETWEEN its PUSH_HANDLER and POP_HANDLER in
    // byte order ("POP_HANDLER belongs only to the normal-completion path"
    // — compiler.cpp), at a LOWER active-handler depth than the protected
    // region around it (THROW already removed the record before jumping
    // there) — a pure byte-order reading gets this wrong. A backend (S6,
    // #459) uses this to know, statically, which compiled catch block a
    // fallible op at this offset must branch to when the runtime reports
    // the fault resolved at this function's own frame.
    std::vector<int> activeHandler;
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
