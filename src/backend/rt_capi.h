#pragma once

// The QBE backend's C interface to the runtime (Layer 3,
// notes/qbe-backend.md). QBE only speaks the C calling convention, so
// everything compiled code calls crosses this boundary: every function
// below is `extern "C"` and `noexcept`, and turns any C++ exception it
// meets into RT_OP_FATAL rather than letting it reach compiled code, which
// writes no unwinding tables to run one through (Q2).
//
// Value is the one non-scalar type this header hands across that boundary.
// It is ABI-safe to do so only because of the static_assert in rt_capi.cpp:
// a NaN-tagged Value is a single 8-byte word, so the C calling convention
// passes and returns it exactly like a raw uint64_t/pointer — the same `l`
// slot QBE already uses for the values compiled code keeps in its own stack
// slots. This header is unusable, and libloxrt.a refuses to build, with
// LOXPP_NAN_TAGGING off (Q6).
//
// A hand-written QBE .ssa never includes this header — it has no C++
// preprocessor — so it is not itself part of the ABI. It documents the ABI
// for the two audiences that do compile C++ against it: the (future) QBE
// emitter's own support code, and this node's own tests.

#include "value.h"
#include "chunk_decoder.h"

#include <cstddef>
#include <cstdint>

class Runtime;

// Value is a POD single 8-byte word (enforced by rt_capi.cpp's
// static_assert), so returning it by value across the C ABI is safe despite
// being a C++ class type — this only silences the compiler's generic
// warning about that shape, which does not know that fact.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type-c-linkage"
#endif

extern "C" {

// One function's expected shape, as an emitter recorded it at compile
// time. rt_startup() matches these against the ObjFunction tree rebuilt
// from the embedded source at process start, by id (chunk_decoder.h);
// `code` is attached to the matching ObjFunction only once arity and
// chunkHash both agree.
struct RtFunctionDesc {
    const char* id; // decodeFunctionTree() id, e.g. "0" or "0.1.2"
    int arity;
    uint64_t chunkHash; // hashChunkBytes() over the function's own chunk
    void* code;         // RtCompiledFn (backend/rt_abi.h), stored as void*
                        // so this struct has no C++-only member type
};

// Embeds and recompiles `source` (the whole program, exactly as an
// emitter's --target qbe invocation compiled it), defines the stdlib, and
// verifies+attaches `descs` against the rebuilt ObjFunction tree. Returns a
// heap-allocated Runtime on success; on a compile error or any desc
// mismatch, writes a diagnostic to stderr and returns nullptr. The caller
// owns the returned Runtime and must release it with rt_shutdown().
Runtime* rt_startup(const char* source, const RtFunctionDesc* descs,
                    std::size_t nDescs) noexcept;

void rt_shutdown(Runtime* rt) noexcept;

// Raw stack access. Compiled code writes its own slots directly at
// base + 8h (base from a CallFrame's `slots`, or rt_stack_base() for the
// outermost call) rather than going through push()/pop() one value at a
// time; rt_set_top() is how it tells this Runtime where its own top now is
// before any call that can allocate or unwind (Q1).
void rt_push(Runtime* rt, Value value) noexcept;
Value rt_pop(Runtime* rt) noexcept;
Value rt_peek(Runtime* rt, int distance) noexcept;
Value* rt_top(Runtime* rt) noexcept;
void rt_set_top(Runtime* rt, Value* top) noexcept;
Value* rt_stack_base(Runtime* rt) noexcept;
int rt_frame_count(Runtime* rt) noexcept;

// Interns `chars` and returns it as a Value. Not one of the op*() wrappers
// below — it exists so this node's own checkpoint can construct a string
// to print without a compiled CONSTANT op (that lowering is straight-line
// code's job).
Value rt_new_string(Runtime* rt, const char* chars) noexcept;

// Looks up a global by name (Runtime::getGlobal), returning Nil if it is
// undefined. GET_GLOBAL's own compiled lowering (straight-line code) has a
// real undefined-global error path; this convenience does not — it exists
// so this node's checkpoint can reach a stdlib function (e.g. "clock",
// defined by every embedded program's own defineNatives() call) with no
// compiled GET_GLOBAL op to do it through yet.
Value rt_get_global(Runtime* rt, const char* name) noexcept;

// PRINT's own stack effect (stringify the popped value, write it plus a
// newline to stdout), reused here ahead of PRINT's own compiled lowering:
// PRINT stays inline in VM::run() (see runtime.h's file comment on which
// opcodes moved to Runtime's op*() methods) and this node's checkpoint
// still needs to prove a string reaches stdout through the whole toolchain.
int rt_op_print(Runtime* rt) noexcept;

// The QBE-specific call path: dispatches on the callee's type exactly as
// VM::run()'s Op::CALL does (native, closure, bound method, bound native,
// class, enum constructor). When the callee is a closure whose code is
// already attached, this pushes the CallFrame and calls straight into that
// code instead of leaving the frame for an interpreter to run.
int rt_call(Runtime* rt, int argCount) noexcept;

// One wrapper per Runtime::op*() method (runtime.h) — the rest of the
// polymorphic-dispatch opcodes CALL's own family does not cover.
int rt_op_invoke(Runtime* rt, ObjString* name, int argCount,
                 int stopAtFrameCount) noexcept;
int rt_op_get_property(Runtime* rt, ObjString* name,
                       int stopAtFrameCount) noexcept;
int rt_op_get_super(Runtime* rt, ObjString* name) noexcept;
int rt_op_super_invoke(Runtime* rt, ObjString* name, int argCount,
                       int stopAtFrameCount) noexcept;
int rt_op_inherit(Runtime* rt) noexcept;
int rt_op_get_index(Runtime* rt, int stopAtFrameCount) noexcept;
int rt_op_set_index(Runtime* rt, int stopAtFrameCount) noexcept;
int rt_op_get_iter(Runtime* rt, int stopAtFrameCount) noexcept;
int rt_op_iter_has_next(Runtime* rt) noexcept;
int rt_op_iter_next(Runtime* rt) noexcept;

} // extern "C"

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// Not part of the extern "C" ABI: a C++-only helper used by rt_startup()
// and, directly, by this node's own tests. Verifies `desc` against the
// node in `root`'s tree named by `desc.id` (arity, then hashChunkBytes())
// and attaches `desc.code` to that ObjFunction on success. Returns false,
// after writing a diagnostic to stderr, on a missing id or any mismatch.
bool rt_attach_code(const DecodedFunction& root,
                    const RtFunctionDesc& desc) noexcept;
