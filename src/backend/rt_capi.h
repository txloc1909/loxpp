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
// slots. The Value is always NaN-boxed, so this holds on every build.
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
//
// `requireAllCompiled` (S4, #457, hazard found reviewing S2's PR #473 —
// tagged R4 on that PR's inline review comments): when true, rt_startup
// additionally walks the WHOLE rebuilt function tree and fails (with a
// diagnostic naming the missing function's id) unless every function in
// it got code attached. Calling an
// interpreted-fallback closure (function->code == nullptr) from compiled
// code pushes a CallFrame nobody ever interprets — VM::run()'s dispatch
// loop, opCall()'s only consumer that can drain it, never runs in a
// --target qbe binary — leaving the Runtime with a dangling frame and no
// diagnostic (Runtime::OpResult::Resumed is a status meaningful only
// inside that loop). Defaults to false so existing callers (this node's
// own unit tests, S2/S3's checkpoint harnesses, each of which
// deliberately compiles only part of a program) are unaffected; a
// whole-program driver — S4's own checkpoint harness, and any later
// `--target qbe` front end — should pass true.
Runtime* rt_startup(const char* source, const RtFunctionDesc* descs,
                    std::size_t nDescs,
                    bool requireAllCompiled = false) noexcept;

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

// Sets the command-line arguments the program sees through args()
// (spec/05-stdlib.md). The generated harness calls this once after
// rt_startup() with argv + 1 and argc - 1, so a standalone binary's own
// name is dropped and its remaining arguments match what `loxpp script.lox
// alpha beta` exposes as [alpha, beta].
void rt_set_args(Runtime* rt, int argc, const char* const* argv) noexcept;

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

// PRINT's own stack effect (stringify the popped value — dispatching __str__ —
// then write it plus a newline to stdout), routed through Runtime::opStr so
// print and str() agree on the canonical form. Takes the enclosing run()'s
// boundary because a __str__ dispatch can throw a catchable error.
int rt_op_print(Runtime* rt, int stopAtFrameCount) noexcept;

// The QBE-specific call path: dispatches on the callee's type exactly as
// VM::run()'s Op::CALL does (native, closure, bound method, bound native,
// class, enum constructor). When the callee is a closure whose code is
// already attached, this pushes the CallFrame and calls straight into that
// code instead of leaving the frame for an interpreter to run.
//
// `stopAtFrameCount` (S6, #459) defaults to 0 so every existing caller
// outside compiled code (this node's own unit tests, each checkpoint
// harness's top-level `rt_call(rt, 0)`) keeps compiling unchanged — a
// top-level call from C++ has no ambient compiled frame, so 0 is the right
// boundary. A compiled function's own CALL lowering (qbe_emitter.cpp)
// always passes its own per-function boundary explicitly (its own frame
// depth minus one) so the returned status's Resumed/Stop distinction tells
// it whether the throw resolved at ITS OWN frame or somewhere else — see
// rt_abi.h's own comment on RtCompiledFn's three-way contract.
int rt_call(Runtime* rt, int argCount, int stopAtFrameCount = 0) noexcept;

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
int rt_op_iter_has_next(Runtime* rt, int stopAtFrameCount) noexcept;
int rt_op_iter_next(Runtime* rt) noexcept;

// Classes, methods, aggregates, slicing, and match dispatch (S5, #458).
// rt_op_class/rt_op_define_method have no error path, matching
// rt_op_define_global's shape above.
int rt_op_class(Runtime* rt, ObjString* name) noexcept;
int rt_op_set_property(Runtime* rt, ObjString* name) noexcept;
int rt_op_define_method(Runtime* rt, ObjString* name) noexcept;
int rt_op_build_list(Runtime* rt, int count) noexcept;
int rt_op_build_map(Runtime* rt, int count, int stopAtFrameCount) noexcept;
int rt_op_slice(Runtime* rt, int stopAtFrameCount) noexcept;
int rt_op_get_tag(Runtime* rt) noexcept;
// S8 (#461), P8 (bytecode-translation-problems.md): GET_TAG;JUMP_TABLE
// fusion. Reads the enum Value at *slot and returns its constructor tag as
// a word, WITHOUT touching the operand stack — unlike rt_op_get_tag above,
// which replaces the top with a boxed Number. A dense enum match lowers the
// pair as one dispatch, so the Number (and the JUMP_TABLE's own `d cast` +
// `dtosi` back to a word) never needs to exist. Returns -1 after
// runtimeError() when *slot is not an enum, the same fatal error
// Runtime::opGetTag raises; every real enum tag is a non-negative
// ObjEnumCtor::tag.
int rt_get_tag_word(Runtime* rt, Value* slot) noexcept;
int rt_op_match_error(Runtime* rt, int stopAtFrameCount) noexcept;

// NOT/IS_SEQ/INSTANCEOF (S7, #460): the last 3 opcodes with no owner in
// the QBE emitter — no earlier node's own checkpoint happened to exercise
// them. All 3 have no error path, matching rt_op_class's shape above.
int rt_op_not(Runtime* rt) noexcept;
int rt_op_is_seq(Runtime* rt) noexcept;
int rt_op_instanceof(Runtime* rt, ObjString* className) noexcept;

// rt_op_in/rt_op_len: the wrappers T1 (#465) and T3 (#467) left for this
// node to add, since Runtime::opIn/opLen already existed but had no QBE-
// callable entry point yet (issue #472's cross-mission coordination
// comment on #462).
int rt_op_in(Runtime* rt, int stopAtFrameCount) noexcept;
int rt_op_len(Runtime* rt, int stopAtFrameCount) noexcept;
int rt_op_str(Runtime* rt, int stopAtFrameCount) noexcept;

// One wrapper per Runtime::op*() arithmetic/comparison helper (runtime.h,
// moved out of vm.cpp by the operator-overloading mission's T1 node). Each
// is the slow path only: the emitter (backend/qbe_emitter.h) inlines the
// plain double-double fast path itself (Q5, notes/qbe-backend.md) and
// calls these only once that fast path has already failed — the same
// division of labor VM::run()'s own inline fast path/dispatchOp split
// uses. No rt_op_in: IN is S5's opcode, not S3's (notes/qbe-backend.md,
// "Staged plan").
int rt_op_add(Runtime* rt, int stopAtFrameCount) noexcept;
int rt_op_subtract(Runtime* rt, int stopAtFrameCount) noexcept;
int rt_op_multiply(Runtime* rt, int stopAtFrameCount) noexcept;
int rt_op_divide(Runtime* rt, int stopAtFrameCount) noexcept;
int rt_op_modulo(Runtime* rt, int stopAtFrameCount) noexcept;
int rt_op_negate(Runtime* rt, int stopAtFrameCount) noexcept;
int rt_op_less(Runtime* rt, int stopAtFrameCount) noexcept;
int rt_op_greater(Runtime* rt, int stopAtFrameCount) noexcept;
int rt_op_equal(Runtime* rt, int stopAtFrameCount) noexcept;

// One wrapper per Runtime's global-variable helper (runtime.h). `name` must
// already be interned (rt_new_string/MemoryManager::makeString dedupe by
// content, so a compiled name literal reaches the same ObjString* the
// startup recompile's own DEFINE_GLOBAL used) — the emitter is responsible
// for interning it before calling these, exactly the way it is responsible
// for setting rt_set_top() first (Q1).
int rt_op_define_global(Runtime* rt, ObjString* name) noexcept;
int rt_op_get_global(Runtime* rt, ObjString* name,
                     int stopAtFrameCount) noexcept;
int rt_op_set_global(Runtime* rt, ObjString* name,
                     int stopAtFrameCount) noexcept;

// CALL/RETURN/CLOSURE/upvalues/CLOSE_UPVALUE (S4, #457). No capture
// analysis at this stage (notes/qbe-backend.md, "Staged plan", row S4):
// every one of these reaches the VM's own existing captureUpvalue/
// closeUpvalues mechanism unchanged, the same code path VM::run()'s
// Op::CLOSURE/GET_UPVALUE/SET_UPVALUE/CLOSE_UPVALUE cases already use.

// The currently executing frame's own closure, as a Value — Runtime::
// currentClosure(). base[0] holds this only when the frame was entered by
// a direct CALL on a closure Value; a method's own frame (init(), INVOKE,
// SUPER_INVOKE — S5, #458) has the receiver ("this") at base[0] instead,
// so compiled code cannot reach its own closure by reading its stack
// window the way CLOSURE/GET_UPVALUE/SET_UPVALUE/CONSTANT once assumed
// (notes/qbe-backend.md, "The central design choice", was about the
// window layout, not this). Every one of those reads its own closure via
// this call now, not addr(0).
Value rt_current_closure(Runtime* rt) noexcept;

// Reads constant `constantIndex` from `closure`'s own function's constant
// pool. Compiled code has no constant pool of its own (this header's own
// file comment), so every caller passes rt_current_closure(rt)'s own
// result here — never a Value read out of the frame's own stack window —
// rather than embedding a pointer literal at compile time: the target
// ObjFunction only gets a real address after rt_startup's
// embed-and-recompile step runs, long after this code was emitted.
Value rt_constant_at(Runtime* rt, Value closure, int constantIndex) noexcept;

// Allocates a new ObjClosure over the ObjFunction `functionConstant`
// wraps (a Value from rt_constant_at above). Every upvalue slot the
// target function declares (ObjFunction::upvalueCount) is still nullptr
// on return — the emitter fills each one immediately after, via
// rt_capture_local_upvalue or rt_forward_upvalue below, the same two-step
// vm.cpp's own CLOSURE case runs (create the closure, root it by storing
// it to its own stack slot, then capture each upvalue).
Value rt_new_closure(Runtime* rt, Value functionConstant) noexcept;

// The isLocal=1 case of CLOSURE's own upvalue loop (vm.cpp):
// closure->upvalues[upvalueIndex] = rt->captureUpvalue(localSlot).
void rt_capture_local_upvalue(Runtime* rt, Value closure, int upvalueIndex,
                              Value* localSlot) noexcept;

// The isLocal=0 case: forwards an upvalue the enclosing function already
// captured — closure->upvalues[upvalueIndex] =
// parentClosure->upvalues[parentUpvalueIndex]. Never allocates.
void rt_forward_upvalue(Runtime* rt, Value closure, int upvalueIndex,
                        Value parentClosure, int parentUpvalueIndex) noexcept;

// GET_UPVALUE / SET_UPVALUE's own bodies (vm.cpp). SET_UPVALUE peeks, like
// every other member of the P2 assignment-is-an-expression family — the
// emitter, not this wrapper, is responsible for leaving `v` on the stack.
Value rt_get_upvalue(Runtime* rt, Value closure, int index) noexcept;
void rt_set_upvalue(Runtime* rt, Value closure, int index, Value v) noexcept;

// CLOSE_UPVALUE's own body (vm.cpp): rt->closeUpvalues(last).
void rt_close_upvalues(Runtime* rt, Value* last) noexcept;

// Q1/Q3 (notes/qbe-backend.md): see Runtime::checkStackOverflow's own
// comment (runtime.h) for why compiled code needs this check at all.
// Returns 0 on success; a nonzero return follows the same fatal-vs-
// catchable convention every other rt_op_* wrapper here does.
int rt_check_stack(Runtime* rt, Value* neededTop,
                   int stopAtFrameCount) noexcept;

// --- S6 (#459): status protocol, try/catch, defer, stack overflow --------

// PUSH_HANDLER's own body (Runtime::pushHandler, runtime.h). `checkpointTop`
// is `base + 8*height` at the height PUSH_HANDLER's own static analysis
// gives it (the fused-stack model's own memory address for that height —
// notes/qbe-backend.md's central design choice), the same value the
// interpreted case's `m_rt.stackTop` holds at PUSH_HANDLER time. No error
// path (a bare vector push_back) — void, like rt_close_upvalues.
void rt_push_handler(Runtime* rt, Value* checkpointTop) noexcept;

// POP_HANDLER's own body (vm.cpp): pops the current handler record. An
// empty handler stack here is a BUG (vm.cpp's own RAISE_ERROR), reported
// fatally — no catchable-throw path, matching rt_op_define_method's shape.
int rt_pop_handler(Runtime* rt) noexcept;

// THROW's own body: pops the value to raise (the emitter passes it in
// directly, already loaded — chunk.h documents THROW as "pops the value to
// raise"), then searches for a handler exactly as a runtime fault does
// (Runtime::handleThrowOp). Never returns success (Op::THROW is terminal —
// chunk.h: "control never falls through past THROW"); the emitter's own
// codegen treats any status here as either a local catch or a propagate,
// the same as every other fallible op — see qbe_emitter.cpp.
int rt_throw(Runtime* rt, Value thrownValue, int stopAtFrameCount) noexcept;

// DEFER_RECORD's own body (Runtime::opDeferRecord, runtime.h). No
// catchable error path, matching rt_op_class/rt_op_define_method's shape.
int rt_op_defer_record(Runtime* rt, int argCount) noexcept;

// RUN_DEFERS's own body: pops this frame's own already-lexically-closed
// handler records (Runtime::popHandlersOwnedByCurrentFrame — vm.cpp's own
// comment on Op::RUN_DEFERS explains why: the compiler emits this only
// immediately before RETURN, so any try/catch this frame itself opened is
// already closed by the time it runs), then drains the frame's pending
// defers (Runtime::runPendingDefers). A deferred call's own throw can only
// be caught by an ANCESTOR frame now — never this one, since this frame's
// own handlers are already gone — so a nonzero status here is always a
// propagate, never a local-catch opportunity (qbe_emitter.cpp's own
// Catchability::Propagate).
int rt_run_defers(Runtime* rt, int stopAtFrameCount) noexcept;

// Q4 (notes/qbe-backend.md): Runtime::setCurrentFrameOffset. The QBE
// emitter calls this once before every fallible op, with that op's own
// static bytecode offset, so a stack trace built from a compiled frame
// reports the real fault site instead of that function's first line.
void rt_set_frame_offset(Runtime* rt, int offset) noexcept;

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
