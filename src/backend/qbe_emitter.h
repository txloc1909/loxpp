#pragma once

// QBE code generator (S3, notes/qbe-backend.md — "Staged plan", row S3, and
// hazards Q5/Q8). Covers straight-line code and jumps only: CONSTANT/NIL/
// TRUE/FALSE, POP, GET_LOCAL/SET_LOCAL, DEFINE_GLOBAL/GET_GLOBAL/SET_GLOBAL,
// PRINT, ADD/SUBTRACT/MULTIPLY/DIVIDE/MODULO/NEGATE/LESS/GREATER/EQUAL,
// JUMP/JUMP_IF_FALSE/LOOP, and RETURN's own C-ABI epilogue (rt_abi.h — every
// compiled function needs this, regardless of how simple its body is; the
// CALL-side of RETURN/CLOSURE/upvalues is S4's job, #457). Any other opcode
// throws std::runtime_error naming it, rather than emitting silently wrong
// code — a later node (S4 calls/closures, S5 the rest of the language, S6
// errors) extends the switch this file's own emitInstruction() holds.
//
// Keeps clox's fused stack (notes/qbe-backend.md, "The central design
// choice"): the abstract_stack height at an offset fixes that value's own
// memory slot, `base + 8*height` — no local/temporary split, unlike the JVM
// and CLR backends. One QBE block label per cfg.h leader (reusing
// BasicBlock::label directly, so a block's label agrees with cfg's own).
//
// A chunk with any PUSH_HANDLER throws: a catch-target block's entry state
// is a declared contract (HandlerEntryContract), not something this node's
// generic cfg-successor-driven terminator emission (see qbe_emitter.cpp)
// handles — S6's job (#459).
//
// This node also does not attempt handler-aware unwinding out of a compiled
// function: a slow-path runtime call (rt_op_add, rt_op_get_global, ...) that
// returns anything other than success makes the compiled function return 1
// immediately, the same as a fatal error (rt_abi.h's RtCompiledFn contract).
// When an OUTER, interpreted frame has an active try/catch around a call
// INTO compiled code, and the fault would actually be caught there
// (Runtime::OpResult::Resumed or ::Stop, not ::Fatal), this collapses that
// catch into an uncaught fatal error instead — a known gap, filed against
// S6 (#459) rather than fixed here, since the full status protocol
// (notes/qbe-backend.md, hazard Q2) is that node's own deliverable.

#include "abstract_stack.h"
#include "chunk_decoder.h"

#include <string>

namespace qbe {

// Emits complete QBE textual IL (.ssa) for one function: `data` declarations
// for any global-variable name it references, plus one
// `export function w $<qbeSymbol>(l %rt, l %base) { ... }` matching
// RtCompiledFn's signature (backend/rt_abi.h) exactly. `analysis` must come
// from analyzeStack(fn) (or the matching node of analyzeStackTree(root)) —
// this pass does not recompute it, so a caller driving several functions
// from one tree computes each analysis once and reuses it.
std::string emitScript(const DecodedFunction& fn,
                       const FunctionStackAnalysis& analysis,
                       const std::string& qbeSymbol);

} // namespace qbe
