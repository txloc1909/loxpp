#pragma once

// QBE code generator (S3-S6, notes/qbe-backend.md — "Staged plan", rows
// S3-S6, and hazards Q2-Q5/Q8/P8). Covers straight-line code, jumps, calls/
// closures, the rest of the language, and errors: CONSTANT (Number, and any
// other constant type via rt_constant_at)/NIL/TRUE/FALSE, POP, GET_LOCAL/
// SET_LOCAL, DEFINE_GLOBAL/GET_GLOBAL/SET_GLOBAL, PRINT, ADD/SUBTRACT/
// MULTIPLY/DIVIDE/MODULO/NEGATE/LESS/GREATER/EQUAL/IN/LEN, JUMP/
// JUMP_IF_FALSE/LOOP/JUMP_TABLE (lowered to a compare chain — P8, no QBE
// switch or indirect jump), CALL, CLOSURE/GET_UPVALUE/SET_UPVALUE/
// CLOSE_UPVALUE (no capture analysis — S4 relies on the VM's own
// captureUpvalue/closeUpvalues, unchanged), CLASS/GET_PROPERTY/
// SET_PROPERTY/DEFINE_METHOD/INVOKE/INHERIT/GET_SUPER/SUPER_INVOKE,
// BUILD_LIST/BUILD_MAP/GET_INDEX/SET_INDEX/SLICE, GET_ITER/ITER_HAS_NEXT/
// ITER_NEXT, GET_TAG/MATCH_ERROR, PUSH_HANDLER/POP_HANDLER/THROW/
// DEFER_RECORD/RUN_DEFERS (S6, #459), and RETURN's own C-ABI epilogue
// (rt_abi.h — every compiled function needs this, regardless of how simple
// its body is). Every compiled function's own prologue also checks its
// analyzed max stack height against STACK_MAX (Q3: compiled code writes its
// own frame's slots directly, bypassing push()'s own check entirely — see
// Runtime::checkStackOverflow's comment, runtime.h). Any other opcode
// (Op::NOT, Op::IS_SEQ, Op::INSTANCEOF — no owner anywhere in the Staged
// plan, hazard filed on #460) throws std::runtime_error naming it, rather
// than emitting silently wrong code.
//
// Keeps clox's fused stack (notes/qbe-backend.md, "The central design
// choice"): the abstract_stack height at an offset fixes that value's own
// memory slot, `base + 8*height` — no local/temporary split, unlike the JVM
// and CLR backends. One QBE block label per cfg.h leader (reusing
// BasicBlock::label directly, so a block's label agrees with cfg's own).
//
// S6's own status protocol (Q2): every compiled function's own return value
// is a three-way status (kRtOk/kRtThrow/kRtFatal — rt_abi.h). A fallible
// runtime call whose C signature carries no stopAtFrameCount parameter
// (rt_capi.h) can only ever fail fatally (verified against runtime.cpp: its
// own implementation calls runtimeError() directly, never
// raiseThrowableError) — qbe_emitter.cpp's own Catchability::Fatal. One that
// does take stopAtFrameCount is always called with this function's own
// frame depth minus one (computed once in the prologue), so
// Runtime::OpResult::Resumed means exactly "the throw resolved at MY OWN
// frame": the emitter jumps to the statically active catch block for that
// call site (handler_depth.h's activeHandler — a purely lexical property,
// since PUSH_HANDLER/POP_HANDLER always bracket byte-order-nested, matching
// the compiler's own emission). Any other outcome (OpResult::Stop, or
// Resumed with no statically active handler) propagates kRtThrow to this
// function's own caller, which performs the identical check against ITS OWN
// frame depth — this bubbles a throw back up the physical C call stack one
// compiled frame at a time, with no extra bookkeeping beyond the status
// code itself. RUN_DEFERS never attempts a local catch even when it could
// (Catchability::Propagate): the compiler emits it only immediately before
// RETURN, after this frame's own try/catch regions are already lexically
// closed, so any handler that resolves a deferred call's own throw belongs
// to an ancestor.
//
// A known, documented simplification: Resumed/Stop only unambiguously means
// "resolved at my own frame" / "resolved elsewhere" when handleThrow's own
// unwind resolves in a single, direct pass. A deeply pathological case — a
// deferred call's own throw, itself caught, whose catch body's later code
// happens to recurse back down to exactly this function's own frame depth
// before the whole nested resolution returns — could in principle produce a
// coincidental Resumed this function did not actually own. Not reachable by
// any node's own probes; flagged as a hazard on #460 (the parity gate,
// where a native-vs-QBE differential run is best placed to catch it if it
// ever is).

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
