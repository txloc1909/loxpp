#pragma once

// The QBE backend's calling convention between compiled code and the
// runtime (Layer 3, notes/qbe-backend.md). Kept in its own header, separate
// from rt_capi.h, so runtime.cpp can name the type without pulling in the
// rest of the C interface.

#include "value.h"

class Runtime;

// Signature every QBE-compiled Lox function has. `base` is this call's
// stack window (slot 0 = callee/receiver — the same convention
// Runtime::call() already uses: `slots = stackTop - argc - 1`).
//
// S6 (#459, Q2, notes/qbe-backend.md) gives this a real three-way status
// protocol, matching the design note's own wording ("every fallible call
// returns OK, THROW, or FATAL"):
//
//   kRtOk (0):    success. Return convention (Runtime::callCompiled,
//                 src/runtime.cpp, mirrors Op::RETURN in vm.cpp exactly):
//                 immediately before returning kRtOk, the callee must leave
//                 its return value as the single value on top of the
//                 runtime stack (rt_push()), above every local it
//                 declared — the same "one value at the top" contract
//                 Op::RETURN's own `pop()` relies on. `rt_set_top()` (Q1)
//                 alone is not enough: `callCompiled` pops that value
//                 itself, so it must actually be there. `callCompiled`
//                 then closes any upvalue captured over this call's own
//                 frame slots, collapses the call's stack window, and
//                 pushes the return value at its base — the caller never
//                 sees the callee's locals.
//   kRtThrow (1): a fault propagated OUT of this call's own frame — the
//                 handler (if any) that resolved it is not this frame's
//                 own, so this frame's CallFrame is already gone (unwound
//                 by Runtime::handleThrow as part of resolving it). A
//                 compiled function only ever returns this when NO
//                 statically active PUSH_HANDLER covers the fallible call
//                 site that faulted, or when the runtime reports the fault
//                 resolved somewhere other than this exact frame — see
//                 qbe_emitter.cpp's own local-catch-or-propagate codegen.
//                 The caller must not read anything from this call's own
//                 frame (it may not exist) and must itself return kRtThrow
//                 (propagating one level further) unless the caller's OWN
//                 statically active handler covers ITS OWN call site,
//                 exactly the same decision this callee just made.
//   kRtFatal (2): uncaught, already reported through runtimeError()
//                 (which already called resetStack()) — the whole Runtime
//                 is reset. The caller must propagate kRtFatal immediately,
//                 unconditionally, with no frame-state inspection.
//
// A caller OUTSIDE compiled code (Runtime::callCompiled) translates
// kRtThrow back into the OpResult/stopAtFrameCount convention the rest of
// Runtime already understands, the same way fromThrow() does for every
// other call site — see callCompiled's own comment.
inline constexpr int kRtOk = 0;
inline constexpr int kRtThrow = 1;
inline constexpr int kRtFatal = 2;

using RtCompiledFn = int (*)(Runtime*, Value*);
