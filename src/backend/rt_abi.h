#pragma once

// The QBE backend's calling convention between compiled code and the
// runtime (Layer 3, notes/qbe-backend.md). Kept in its own header, separate
// from rt_capi.h, so runtime.cpp can name the type without pulling in the
// rest of the C interface.

#include "value.h"

class Runtime;

// Signature every QBE-compiled Lox function has. `base` is this call's
// stack window (slot 0 = callee/receiver — the same convention
// Runtime::call() already uses: `slots = stackTop - argc - 1`). Returns 0
// on success; a nonzero return means the callee already reported a fatal
// error, the same contract a failing native call already has
// (Runtime::callNative, src/runtime.cpp).
//
// Return convention on success (Runtime::callCompiled, src/runtime.cpp,
// mirrors Op::RETURN in vm.cpp exactly): immediately before returning 0,
// the callee must leave its return value as the single value on top of the
// runtime stack (rt_push()), above every local it declared — the same
// "one value at the top" contract Op::RETURN's own `pop()` relies on.
// `rt_set_top()` (Q1) alone is not enough: `callCompiled` pops that value
// itself, so it must actually be there. `callCompiled` then closes any
// upvalue captured over this call's own frame slots, collapses the
// call's stack window, and pushes the return value at its base — the
// caller never sees the callee's locals.
using RtCompiledFn = int (*)(Runtime*, Value*);
