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
using RtCompiledFn = int (*)(Runtime*, Value*);
