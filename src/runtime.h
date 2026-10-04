#pragma once

// Runtime — the VM's state, and every operation on that state that does not
// need the interpreter dispatch loop's own register-cached ip/frame/chunk
// locals. VM::run() (vm.h/vm.cpp) is the interpreter loop: it stays a
// separate class because caching ip/frame/chunk in local variables (rather
// than re-reading them from Runtime on every instruction) is what makes the
// dispatch loop fast, and only the loop itself needs them.
//
// push/pop/peek, call(), and the op*() opcode helpers below take only the
// operands their opcode already decoded from the bytecode stream (a
// constant-pool name, an argument count) — never ip/chunk. That keeps this
// class usable by any caller that owns its own program counter, not only
// VM::run()'s switch; see notes/qbe-backend.md for the design this shape
// supports.
//
// A slice of opcode bodies — the ones with the most runtime polymorphism:
// CALL/INVOKE/SUPER_INVOKE dispatch, GET_PROPERTY/GET_SUPER, INHERIT,
// GET_INDEX/SET_INDEX, and the iterator ops — is implemented as the op*()
// methods below. Arithmetic, locals/globals, control flow, RETURN, and
// defer/handler bookkeeping are implemented inline in VM::run() instead,
// reached through the friendship grant below.
//
// VM::run() needs raw access to this class's own bookkeeping (m_frames,
// stack/stackTop, m_handlerStack, ...) for the opcodes it implements
// inline, so VM is a friend rather than going through a pile of one-off
// accessors that would only exist to satisfy that one caller. The op*()
// methods and the other operations below are public so that a caller other
// than VM can reach them without being a friend.

#include "exec_objects.h"
#include "class_objects.h"
#include "memory_manager.h"
#include "table.h"
#include "vm_limits.h"
#include "stdlib/stdlib_context.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#ifdef LOXPP_PROFILE
#include "profiler.h"
#endif

enum class InterpretResult : std::uint8_t {
    OK,
    COMPILE_ERROR,
    RUNTIME_ERROR,
};

struct ObjMap;        // container_objects.h; only a pointer is needed here.
struct ObjCoroutine;  // container_objects.h; only a pointer is needed here.
class DiagnosticSink; // diagnostic.h; only a pointer is needed here.

struct HandlerRecord {
    int frameCount;                // number of frames at push time
    Value* stackTop;               // stack pointer at push time
    Chunk::const_iterator catchIp; // jump target for catch block
};

struct CallFrame {
    ObjClosure* closure;
    Chunk::const_iterator ip;
    Value* slots; // points into the VM stack at this frame's base slot
};

class Runtime {
  public:
    static constexpr int STACK_MAX = loxpp::kStackMax;
    static constexpr int FRAMES_MAX = loxpp::kFramesMax;

    // Extra capacity held ABOVE the two ceilings above, spent only while a
    // StackOverflowError's own unwind is in progress (see
    // m_unwindingStackOverflow). Both overflow guards (the frame-count check
    // in call(), the value-stack check in push()) still fire exactly at
    // FRAMES_MAX/STACK_MAX, for every program, handler or not — this reserve
    // is not subtracted from that threshold, so an open try/catch never
    // changes how deep a program that does not overflow can go. The room it
    // buys is spent afterward: closing upvalues, draining each discarded
    // frame's pending defers (each one a real, if short-lived, nested call()
    // — see handleThrow()/runPendingDefers()), and allocating the Error
    // object. Measured directly against this reserve (test_vm_runtime.cpp's
    // StackOverflowTest suite): one deferred call with a short (non-
    // recursive) body needs one CallFrame and a handful of value-stack
    // slots; a chain of ordinary calls inside that body costs more of each.
    // A deferred call that outruns this reserve hits the true hard ceiling
    // (FRAMES_MAX/STACK_MAX plus this reserve) instead — fatal, or caught by
    // whatever handler still has room, per handleThrow()'s existing generic
    // unwind — never a hang, since the reserve is small enough that no path
    // through it can recurse for long.
    static constexpr int STACK_OVERFLOW_FRAME_RESERVE =
        loxpp::kStackOverflowFrameReserve;
    static constexpr int STACK_OVERFLOW_STACK_RESERVE =
        loxpp::kStackOverflowStackReserve;

    Runtime() : m_globals(VmAllocator<Entry>{&m_mm}) {
        resetStack();
        m_mm.setMarkRootsCallback([this]() { markRoots(); });
#ifdef LOXPP_PROFILE
        m_mm.setProfilerData(&m_profilerData);
#endif
        // m_mm's address is fixed for this object's lifetime, so this is a
        // one-time invariant, not something VM::interpret() needs to redo on
        // every REPL line — unlike setActiveContext(&m_stdlibCtx), which
        // VM::interpret() still sets on every call, since that points a
        // global/thread-local at whichever VM is currently active.
        m_stdlibCtx.mm = &m_mm;
        m_stdlibCtx.rt = this;
    }

    // Set once by VM's constructor to `[this](int stop){ return run(stop); }`
    // — the one place this class needs the interpreter loop back
    // (runPendingDefers() must run a deferred call to completion, which
    // means dispatching bytecode). Mirrors the MemoryManager's own
    // setMarkRootsCallback(): a callback, not a direct dependency on VM, so
    // this header never needs to know VM exists.
    void setInterpretLoop(std::function<InterpretResult(int)> loop) {
        m_runLoop = std::move(loop);
    }

    // Outcome of a throw/catch dispatch (handleThrow/raiseThrowableError) or
    // of call() — the two used to be separate enums (CallOutcome/
    // ThrowOutcome) with identical Uncaught/HandledContinue-or-CaughtContinue/
    // HandledStop-or-CaughtStop cases and a hand-written converter between
    // them; call()'s arity/overflow checks already just relabeled whatever
    // ThrowOutcome they got from raiseThrowableError(), so Pushed is the only
    // case call() ever adds. stopAtFrameCount always names the boundary of
    // whichever run() invocation is currently, actually dispatching — the
    // run() parameter itself for Op::THROW and the tryCatchableError lambda,
    // and the same value threaded down through runPendingDefers/call() for a
    // reentrant fault inside a running defer.
    //   Uncaught:        no handler found, runtimeError() already called —
    //                     caller returns InterpretResult::RUNTIME_ERROR.
    //   HandledContinue: a handler was found (here, or by a reentrant call
    //                     several levels down) and the resulting
    //                     m_frameCount is still above the caller's own
    //                     stopAtFrameCount — the caller's own frame context
    //                     is still live; it must FrameSync::loadTop (or let
    //                     its own ambient FrameSync guard do so) and
    //                     continue dispatch normally.
    //   HandledStop:      handled, but the resulting m_frameCount is at or
    //                     below the caller's own stopAtFrameCount — control
    //                     now belongs to a different, less-nested run()
    //                     invocation. The caller must NOT read frame/ip/
    //                     chunk (m_frameCount may even be 0, making that
    //                     read out of bounds) and must return
    //                     InterpretResult::OK immediately.
    //   Pushed:           call() only — a new frame is on top of m_frames;
    //                     proceed normally, the same as HandledContinue.
    enum class ThrowOutcome : std::uint8_t {
        Uncaught,
        HandledContinue,
        HandledStop,
        Pushed,
    };

    // Outcome of an op*() opcode helper below. These helpers can allocate,
    // call (pushing a new CallFrame), and throw (unwinding zero or more
    // frames) — OK, Resumed, Stop and Fatal collapse ThrowOutcome above into
    // the four shapes a caller actually has to react to differently:
    //   OK:      plain success — frame/ip/chunk are exactly what the caller
    //            already has; no reload needed. An op*() only returns this
    //            literally, never via fromThrow — that covers a path that
    //            can touch m_frames.
    //   Resumed: the helper pushed a call frame (ThrowOutcome::Pushed) or an
    //            error was caught within the caller's own run() invocation
    //            (ThrowOutcome::HandledContinue) — m_frames/m_frameCount
    //            already reflect it; the caller must reload frame/ip/chunk
    //            from the current top before resuming dispatch.
    //   Stop:    resolved by a handler outside the caller's own run()
    //            invocation (ThrowOutcome::HandledStop). Caller returns
    //            InterpretResult::OK immediately without touching
    //            frame/ip/chunk.
    //   Fatal:   uncaught error already reported via runtimeError(). Caller
    //            returns InterpretResult::RUNTIME_ERROR immediately.
    enum class OpResult : std::uint8_t { OK, Resumed, Stop, Fatal };

    // White-box seam for StackOverflowTest and VM's own interpreter loop.
    friend class VM;
    friend struct VMTestAccess;

    void resetStack();

    // Compiles `source`, defines the stdlib, and wraps the result in an
    // ObjClosure rooted on the stack at slot 0 — the shared first half of
    // VM::interpret() and rt_startup() (backend/rt_capi.cpp, the QBE
    // backend's embed-and-recompile startup path). Neither caller needs
    // bytecode dispatch for this part: it stops short of pushing the first
    // CallFrame (call(closure, 0)) so a compile error and an uncaught
    // arity/overflow fault from that call stay distinguishable outcomes to
    // the caller, the same distinction VM::interpret() already made.
    // Returns nullptr on a compile error (already reported to stderr, or
    // collected in `sink` when one is given).
    ObjClosure* loadSource(const std::string& source,
                           DiagnosticSink* sink = nullptr);

    // Defined inline (not in runtime.cpp): these are called on every single
    // opcode dispatch, and vm.cpp is a separate translation unit from
    // runtime.cpp with no LTO — an out-of-line definition here measured
    // roughly 4x slower on a call-heavy benchmark (fib(32), release preset)
    // because the compiler could no longer inline them into VM::run()'s
    // loop. Keep the operand-stack primitives header-only so any TU that
    // includes runtime.h gets the same inlining that the single-file VM
    // used to get for free — see notes/qbe-backend.md for why that matters
    // to more than just vm.cpp.
    void push(Value value) {
        // Same threshold, STACK_MAX, whether or not a handler is active (see
        // STACK_OVERFLOW_STACK_RESERVE's own comment above): an open
        // try/catch must never change how deep a program that does not
        // overflow can go. While unwinding a StackOverflowError
        // (m_unwindingStackOverflow), the ceiling moves out to STACK_MAX +
        // STACK_OVERFLOW_STACK_RESERVE — the physical capacity `stack`
        // actually has — so a deferred call drained during that unwind can
        // use the reserve; past that, it stays fatal.
        std::ptrdiff_t hardCeiling =
            STACK_MAX +
            (m_unwindingStackOverflow ? STACK_OVERFLOW_STACK_RESERVE : 0);
        // Use >=, not ==: the ceiling moves with m_unwindingStackOverflow,
        // and stackTop only grows, so a pointer already past STACK_MAX when
        // the flag clears would never hit an exact match again.
        if (stackTop >= stack + hardCeiling) {
            m_stackOverflow = true;
            return;
        }
        *stackTop++ = value;
    }
    Value pop() { return *--stackTop; }
    Value peek(int distance) { return stackTop[-1 - distance]; }

    // Compiled code (backend/rt_capi.cpp) writes its own stack slots
    // directly at base + 8h instead of going through push(), so it needs a
    // way to tell this Runtime where its own top now is before any call
    // that can allocate or unwind — see notes/qbe-backend.md, hazard Q1.
    // Neither accessor re-checks STACK_MAX: the compiled code that computed
    // `top` already knows its own height, the same way push()'s caller does
    // not re-check a height it already holds.
    [[nodiscard]] Value* top() const { return stackTop; }
    void setTop(Value* newTop) { stackTop = newTop; }

    // Base of the value stack — slot 0 of the outermost call. Compiled code
    // needs this once, at startup, to compute its own initial stack window;
    // every call after that gets its window from the CallFrame the runtime
    // hands back (frame->slots).
    [[nodiscard]] Value* stackBase() { return stack; }

    // Runs pending defers for m_frames[frameIndex] LIFO, each to completion
    // (via a nested run() call) before the next one starts, so ordering and
    // side effects land exactly as multiple sequential calls would. Used by
    // Op::RUN_DEFERS and by handleThrow's unwind loop for each discarded
    // frame. stopAtFrameCount is passed straight through to this frame's own
    // deferred call() (see ThrowOutcome above) — it is NOT frameIndex: a
    // handler reachable while draining frameIndex's own defers was always
    // pushed before frameIndex's function was even called (any handler
    // scoped inside that function's own body is already popped by the time
    // RUN_DEFERS runs), so catching one always means frameIndex itself no
    // longer exists, regardless of stopAtFrameCount. RUNTIME_ERROR propagates
    // a hard error at the call site itself (arity mismatch, stack overflow);
    // on OK, the caller must still check whether m_frameCount is still
    // frameIndex + 1 — a lower value means a deferred call's own throw (or
    // arity mismatch) escaped past this frame, and the caller must stop
    // unwinding/dispatching at its own level too rather than assume frame
    // `frameIndex` is still live.
    InterpretResult runPendingDefers(int frameIndex, int stopAtFrameCount);

    ThrowOutcome call(ObjClosure* closure, int argCount,
                      int stopAtFrameCount = 0);
    OpResult callNative(ObjNative* native, int argCount, int stopAtFrameCount);
    OpResult callBoundNative(ObjBoundNative* bn, int argCount,
                             int stopAtFrameCount);
    bool bindMethod(ObjClass* klass, ObjString* name);
    void defineNatives();

    // Synchronously invokes the callable whose callee and `argCount` arguments
    // already sit at the top of the operand stack (callee at
    // stackTop[-argCount-1]), running an interpreted callee to completion via
    // a nested run(). On success, pops the result into *out, restores the
    // stack top, and returns true. Returns false when the callee's throw was
    // resolved outside this invocation or was uncaught; callNative() then
    // propagates the recorded outcome instead of a result. This is the bounded
    // re-entrant call path a native such as callMethod uses to reach a
    // closure-backed method (notes/expressiveness-roadmap.md item 1).
    bool invokeCallableFromNative(int argCount, Value* out);

    // The method-invocation variant of invokeCallableFromNative: `method` has
    // already been resolved from a class's method table, and `this` must be
    // the receiver at stackTop[-argCount-1] — the caller pushes the receiver
    // and the forwarded args before calling. Binds slot 0 to that receiver
    // exactly as Op::INVOKE does, then runs the call to completion. On success
    // pops the result into *out and returns true; otherwise records the
    // outcome for callNative() and returns false.
    bool invokeMethodFromNative(ObjClosure* method, int argCount, Value* out);

    // Resumes a suspended coroutine. On the first resume the coroutine's
    // callee and `argCount` arguments sit at the top of the operand stack
    // (callee at stackTop[-argCount-1]); on later resumes `argCount` is 0 or
    // 1 and the single argument (or nil) becomes the pending yield's value.
    // On success writes the value the coroutine next yields or returns to
    // *out and returns true. On a throw that left the coroutine (dead) or a
    // resume-state fault, records the outcome in m_reentrantOutcome and
    // returns false, exactly as invokeCallableFromNative does.
    bool resumeCoroutine(ObjCoroutine* co, int argCount, Value* out);

    // Suspends m_currentCoroutine, copying its live interpreter state into the
    // coroutine's snapshot. Called by Op::YIELD after the yielded value has
    // been popped and frame->ip flushed.
    void suspendCurrentCoroutine(Value yielded);

    // Native-facing reach for the shared Coroutine class.
    [[nodiscard]] ObjClass* coroutineClass() const { return m_coroutineClass; }

    // Native-facing key operations for map.has / map.del. They run the key
    // through mapGetKey / mapDelKey with the native's enclosing boundary, and
    // on failure store the outcome in m_reentrantOutcome for callNative() to
    // propagate (the native returns a placeholder). Return true on success.
    bool mapHasFromNative(ObjMap* map, const Value& key, bool* out);
    bool mapDelFromNative(ObjMap* map, const Value& key);

    // The two throwable checks a map key must pass (NaN key, non-string
    // object key) — kind/message pair, or nullopt when indexVal is valid.
    // Shared by every map-key check site: opGetIndex/opSetIndex below (via
    // checkMapKey(), which wraps this for the op*()/OpResult convention)
    // and VM::run()'s own BUILD_MAP/Op::IN cases (which raise it through
    // tryCatchableError() instead, the convention opcodes still inline in
    // VM::run() use). One rule, two callers each wrapping it their own way.
    struct MapKeyError {
        const char* kind;
        const char* message;
    };
    std::optional<MapKeyError> mapKeyError(Value indexVal);

    // Shared by opGetIndex's and opSetIndex's map branches — raises
    // mapKeyError() above through the op*() convention. Returns nullopt
    // when indexVal is a valid map key; otherwise the OpResult the caller
    // should return immediately.
    std::optional<OpResult> checkMapKey(Value indexVal, int stopAtFrameCount);

    // True when `v` is a valid map key: nil, bool, a non-NaN number, a string,
    // or an Instance whose class defines both __hash__ and __eq__. This is the
    // VM-aware extension of isValidMapKey() (value.cpp), which covers only the
    // scalar/string part.
    bool mapKeyValid(const Value& v) const;

    // A map key's bucket hash. A scalar/string key uses hashValue(); an
    // Instance key calls __hash__ synchronously (ResultCheck::Number, so a
    // non-Number result raises OperatorResultTypeError). A NaN result is
    // rejected as NaNKeyError. Returns nullopt and records the outcome in
    // m_mapKeyStatus on failure.
    std::optional<uint32_t> hashMapKey(const Value& key, int stopAtFrameCount);

    // The key-equality callback for the VM map path. `stored` is the key
    // already in the map (the __eq__ receiver, CPython-style); `lookup` is the
    // key the program supplied. A scalar/string pair uses operator==. On a VM
    // failure this sets m_mapKeyStatus and returns false; the caller checks
    // m_mapKeyStatus after the map call.
    bool mapKeyEq(const Value& stored, const Value& lookup,
                  int stopAtFrameCount);

    // Map key operations. Each locks `map` across hashing and probing, so a
    // user __hash__/__eq__ that writes to the same map raises MapChangedError.
    // On success *found says whether the key is present. Returns the OpResult
    // to propagate: OK, or Stop/Fatal from a throw inside __hash__/__eq__.
    OpResult mapGetKey(ObjMap* map, const Value& key, bool& found, Value& out,
                       int stopAtFrameCount);
    OpResult mapSetKey(ObjMap* map, const Value& key, const Value& value,
                       int stopAtFrameCount);
    OpResult mapDelKey(ObjMap* map, const Value& key, int stopAtFrameCount);

    // Shared by opGetIndex's and opSetIndex's List/String branches — the
    // same three throwable checks (must be a number, must be an integer,
    // must be in bounds for a collection of `size` elements), differing
    // only in `noun` ("List"/"String") for the message text. On success,
    // writes the validated index to *outIdx and returns nullopt; on
    // failure, the OpResult the caller should return immediately.
    std::optional<OpResult> checkSequenceIndex(Value indexVal, size_t size,
                                               const char* noun,
                                               int stopAtFrameCount,
                                               int* outIdx);

    // Shared by opIterHasNext's and opIterNext's map branches — reports
    // "Map changed size during iteration." (and returns true) if map's
    // version has moved past what the iterator captured at GET_ITER time;
    // false means iteration may proceed. See ObjIterator::expectedVersion's
    // own comment for why a version check catches what a size check would
    // miss (a paired erase+insert restoring the net size).
    bool mapIterationInvalidated(ObjMap* map, int expectedVersion);
    ObjUpvalue* captureUpvalue(Value* local);
    void closeUpvalues(Value* last);
    // Discards every m_handlerStack record whose frameCount equals the
    // frame at depth m_frameCount (the frame about to be left) — see
    // INVARIANT(handler-stack-frame-scoped) on m_handlerStack's
    // declaration. Called from both Op::RETURN (the only exit for a
    // defer-free function) and Op::RUN_DEFERS (which always runs before
    // Op::RETURN in the same frame, so its own defers must not observe a
    // record this frame no longer owns). A frame with no open region does
    // nothing here; POP_HANDLER already removed its records.
    void popHandlersOwnedByCurrentFrame();
    void runtimeError(const char* format, ...);
    void markRoots();

    // Pushes a handler record for a PUSH_HANDLER at the current frame depth
    // — the interpreted case (vm.cpp) and compiled code's own rt_push_handler
    // (backend/rt_capi.h) both call this, decoding catchOffset their own way
    // first (an interpreter reads it from the bytecode stream; compiled code
    // already knows it statically — see qbe_emitter.cpp). `checkpointTop` is
    // the stack pointer AT PUSH_HANDLER time (the interpreted case's own
    // `m_rt.stackTop`; compiled code's own `base + 8*height`, the fused-
    // stack model's matching memory address for the same height —
    // notes/qbe-backend.md's central design choice). `catchIp` is
    // meaningful only to the interpreter's own dispatch loop (it resumes
    // there directly); compiled code never reads a HandlerRecord's own
    // catchIp back — see qbe_emitter.cpp's own comment on why it derives
    // its catch target from handler_depth's static analysis instead.
    void pushHandler(Value* checkpointTop, Chunk::const_iterator catchIp) {
        m_handlerStack.push_back(
            HandlerRecord{m_frameCount, checkpointTop, catchIp});
    }

    // POP_HANDLER's own body (vm.cpp, backend/rt_capi.h's rt_pop_handler):
    // pops the innermost handler record. Callers must check
    // handlerStackDepth() first — an empty stack here is a BUG (a
    // compiler/decoder invariant violation), reported by the caller, not
    // by this method.
    void popTopHandler() { m_handlerStack.pop_back(); }

    // DEFER_RECORD's own body (vm.cpp/backend/rt_capi.h): pops [callee,
    // arg0, ..., argCount-1] and records them as a pending deferred call on
    // the current frame's own defer list. No catchable error path — the
    // one way this can fail is a stray allocation failure, the same as
    // opClass/opDefineMethod above.
    void opDeferRecord(int argCount);

    // Sets the CURRENT (topmost) frame's own bytecode-offset bookkeeping
    // to `offset` — Q4 (notes/qbe-backend.md): a compiled frame's own `ip`
    // is set once, at call time (Runtime::call), and never advances the way
    // the interpreter's own dispatch loop keeps it current. Without this, a
    // stack trace built from a compiled frame always reports that
    // function's very first line (Chunk::getLine's own fallback on a
    // negative/stale offset), never the real fault site. The QBE emitter
    // calls this (via rt_set_frame_offset, backend/rt_capi.h) once before
    // every fallible op, with that op's own static bytecode offset.
    void setCurrentFrameOffset(int offset) {
        CallFrame& frame = m_frames[m_frameCount - 1];
        frame.ip = frame.closure->function->chunk.cbegin() + offset;
    }

    // Helper for handling a thrown error: searches handler stack LIFO, unwinds
    // frames if found, and updates m_frames/m_frameCount. See ThrowOutcome
    // above for the three possible results and what each obligates the
    // caller to do.
    ThrowOutcome handleThrow(Value thrownValue, int stopAtFrameCount = 0);

    // Helper for raising a catchable runtime error (used by both run()'s
    // tryCatchableError and call()'s arity check). Constructs an Error object
    // with the given kind and message, then calls handleThrow. See
    // ThrowOutcome above.
    ThrowOutcome raiseThrowableError(const char* kind_str, const char* msg,
                                     int stopAtFrameCount = 0);

    // --- op*() opcode helpers --------------------------------------------
    //
    // Each one has the stack effect of the opcode it implements and takes,
    // beyond `this`, only the operands that opcode already decoded from the
    // bytecode stream (a constant-pool name, an argument count) — never
    // ip/chunk. The interpreter loop decodes those operands (it has ip/
    // chunk), flushes the current frame's ip so a runtimeError() raised in
    // here reports the right line, and then calls straight through. Any
    // other caller with its own program counter (see notes/qbe-backend.md)
    // can do the same: decode the operand itself, store its own bytecode
    // offset into the frame, and call the same function.
    //
    // Defined out-of-line (runtime.cpp), unlike push/pop/peek above: each
    // one is a full opcode body, not a one-line primitive the compiler
    // would inline into VM::run()'s loop either way, so the cross-TU call
    // this costs on every CALL/property/index/iterator dispatch is priced
    // into the same release-preset fib(32) measurement that covers the
    // split as a whole (median of 3 runs: 0.243s on this split vs 0.298s
    // single-file), not a separate, unmeasured cost.
    OpResult opCall(int argCount, int stopAtFrameCount);
    OpResult opInvoke(ObjString* name, int argCount, int stopAtFrameCount);
    OpResult opGetProperty(ObjString* name, int stopAtFrameCount);
    OpResult opGetSuper(ObjString* name);
    OpResult opSuperInvoke(ObjString* name, int argCount, int stopAtFrameCount);
    OpResult opInherit();
    OpResult opGetIndex(int stopAtFrameCount);
    OpResult opSetIndex(int stopAtFrameCount);
    OpResult opGetIter(int stopAtFrameCount);
    OpResult opIterHasNext(int stopAtFrameCount);
    OpResult opIterNext();

    // Resumes a coroutine from ITER_HAS_NEXT. resumeCoroutine() is written for
    // the native resume path: it reports its boundary through
    // m_nativeStopAtFrameCount and its failure through m_reentrantOutcome. This
    // wrapper saves both, nominates `stopAtFrameCount` as the boundary, and
    // supplies the callee/argument window resumeCoroutine() expects at the top
    // of the operand stack. Returns OK on a yield or a normal return; otherwise
    // the OpResult to propagate (Stop, Resumed, or Fatal).
    OpResult resumeCoroutineForIteration(ObjCoroutine* co, int stopAtFrameCount,
                                         Value* out);

    // OpCall() plus a nested run() for whatever frame it pushes: the bounded
    // re-entrant call path. `argCount` names a callee/arg window already at
    // the top of the operand stack, exactly as opCall() expects.
    // `throwBoundary` is the frame-count boundary a throw from the call is
    // judged against (see ThrowOutcome). The run boundary — the frame count the
    // nested run() stops at — is this call's own entry frame count. On OK the
    // result replaces the callee+args window. On Stop, control passed to a
    // handler outside the run boundary; propagate without touching the stack
    // top. On Fatal an uncaught error was already reported and the VM stack was
    // reset.
    OpResult reentrantCall(int argCount, int throwBoundary);

    // The exit classifier for a re-entrant dispatch. A dispatch just pushed
    // exactly one frame (m_frameCount == entry + 1); `frameSlots` is that
    // frame's slot 0 (captured as stackTop - argCount - 1 before the dispatch),
    // and `enclosingBoundary` is the enclosing run()'s own stopAtFrameCount.
    // Runs the frame to completion and classifies the exit:
    //   OK      the callee returned normally; its result is the sole value at
    //           frameSlots[0].
    //   Resumed a throw was caught by a handler still inside the enclosing
    //           run()'s range — control was redirected (ip/frame changed), so
    //           the enclosing run must reload and continue rather than treat
    //           this as a result.
    //   Stop    a throw was caught outside the enclosing run's range; control
    //           belongs to a less-nested run().
    //   Fatal   an uncaught error was reported; the VM stack was reset.
    // The Resumed-vs-OK decision uses frameSlots, not only m_frameCount: a
    // normal return leaves exactly one value at frameSlots[0], while a caught
    // throw truncates the stack to the handler's checkpoint (below frameSlots).
    OpResult runReentrantFrame(int entry, Value* frameSlots,
                               int enclosingBoundary);

    // Runs to completion a single frame that a re-entrant dispatch just pushed
    // (the dispatch returned Resumed with m_frameCount == entry + 1). `entry`
    // is the frame count captured before the dispatch. Shared by every
    // re-entrant call site — opCall-based and method-based alike — so the
    // nested-run boundary and Stop/Fatal translation live in one place.
    OpResult runPushedFrameToCompletion(int entry);

    // Invokes the installed interpreter loop at `entry`, returning false if no
    // loop is installed or the run ended in a fatal error. Shared by the two
    // re-entrant exit classifiers above/below.
    bool runNestedLoop(int entry);

    // Classes, methods, aggregates, slicing, and match dispatch (S5, #458),
    // moved out of VM::run() the same way as the op*() methods above so the
    // QBE backend (backend/rt_capi.h) reaches them with no runtime
    // duplication. opClass/opDefineMethod have no error path in VM::run()
    // either, matching opDefineGlobal's shape above.
    void opClass(ObjString* name);
    OpResult opSetProperty(ObjString* name);
    void opDefineMethod(ObjString* name);
    OpResult opBuildList(int count);
    OpResult opBuildMap(int count, int stopAtFrameCount);
    OpResult opSlice(int stopAtFrameCount);
    OpResult opGetTag();
    OpResult opMatchError(int stopAtFrameCount);

    // NOT/IS_SEQ/INSTANCEOF (S7, #460): plain stack ops with no error path
    // (matching opClass/opDefineMethod's shape above) — moved out of
    // VM::run() last because none of S3-S6's own checkpoints happened to
    // exercise them (#460's own hazard comment).
    void opNot();
    void opIsSeq();
    void opInstanceof(ObjString* className);

    // Arithmetic / comparison / containment operators. Each one owns the
    // slow path of its opcode: the built-in number (and for ADD, string) fast
    // path is inlined in VM::run(), and these are called only once that fast
    // path has failed. They then try the operator-overloading method on the
    // operand (see tryBinaryMethod below) and fall back to the same error the
    // opcode raised before operator overloading existed. The QBE backend
    // reuses these, so the method fallback must not live inline in vm.cpp.
    OpResult opAdd(int stopAtFrameCount);
    OpResult opSubtract(int stopAtFrameCount);
    OpResult opMultiply(int stopAtFrameCount);
    OpResult opDivide(int stopAtFrameCount);
    OpResult opModulo(int stopAtFrameCount);
    OpResult opNegate(int stopAtFrameCount);
    OpResult opLess(int stopAtFrameCount);
    OpResult opGreater(int stopAtFrameCount);
    OpResult opEqual(int stopAtFrameCount);
    OpResult opIn(int stopAtFrameCount);
    OpResult opLen(int stopAtFrameCount);

    // Op::STR/Op::PRINT's shared stringify: pops one value and pushes its
    // canonical string (an ObjString), dispatching __str__ on an Instance.
    OpResult opStr(int stopAtFrameCount);

    // Canonical string form of an Instance: dispatches __str__ when the class
    // defines it (the result must be a String, else the catchable
    // OperatorResultTypeError), otherwise "ClassName instance". Called by
    // stringifyObj's INSTANCE case (object.cpp) through getActiveRuntime().
    // On a throw from __str__ it records the outcome in m_stringifyStatus and
    // returns the depth-guard placeholder so opStr() can propagate it.
    std::string stringifyInstanceStr(ObjInstance* instance);

    // True while opStr() is producing a canonical string, so stringifyObj can
    // tell a print/str stringify (dispatch __str__) from an internal one
    // (default form). Read from object.cpp via getActiveRuntime().
    [[nodiscard]] bool canonicalStringifyActive() const {
        return m_stringifyCanonicalDepth > 0;
    }

    // Global-variable access, moved out of VM::run() the same way (Layer 1)
    // so compiled code (the QBE backend) can reach it: compiled code has no
    // constant pool of its own to look a name up in, so DEFINE_GLOBAL/
    // GET_GLOBAL/SET_GLOBAL become callable helpers exactly like opAdd..opIn
    // above. defineGlobal never fails (the opcode has no error path in
    // VM::run() either); the get/set helpers raise the same catchable
    // UndefinedVariableError, through the same raiseThrowableError/
    // stopAtFrameCount convention.
    void opDefineGlobal(ObjString* name) {
        m_globals.set(name, peek(0));
        pop();
    }
    OpResult opGetGlobal(ObjString* name, int stopAtFrameCount);
    OpResult opSetGlobal(ObjString* name, int stopAtFrameCount);

    // Op::THROW's own op*()-shaped entry point: handleThrow() collapsed
    // through fromThrow() (private below) into the same OpResult contract
    // every other op*() call site uses, so Op::THROW can go through
    // dispatchOp() too instead of hand-rolling the outcome switch.
    OpResult handleThrowOp(Value thrownValue, int stopAtFrameCount) {
        return fromThrow(handleThrow(thrownValue, stopAtFrameCount));
    }

    // Runtime state inspection (for testing and debugging).
    [[nodiscard]] int stackDepth() const {
        return static_cast<int>(stackTop - stack);
    }
    [[nodiscard]] int frameCount() const { return m_frameCount; }
    // The currently running frame's own closure — CallFrame::closure, set
    // by call()/callCompiled() when the frame was pushed. Used by the QBE
    // backend's rt_current_closure (backend/rt_capi.h) to reach a compiled
    // function's own constant pool/upvalues: base[0] holds that closure
    // only for a directly-called function, never for a method (base[0]
    // there is the receiver, "this" — S5, #458), so compiled code cannot
    // read it back out of its own stack window the way CLOSURE/GET_UPVALUE/
    // CONSTANT once assumed.
    [[nodiscard]] ObjClosure* currentClosure() const {
        return m_frameCount > 0 ? m_frames[m_frameCount - 1].closure : nullptr;
    }
    [[nodiscard]] int handlerStackDepth() const {
        return static_cast<int>(m_handlerStack.size());
    }
    [[nodiscard]] std::optional<Value> getGlobal(const std::string& name) const;
    [[nodiscard]] Value lastResult() const { return m_lastResult; }

    // Compiled code needs the allocator directly to build strings, lists,
    // and maps with no VM::run() opcode wrapping it (backend/rt_capi.cpp).
    [[nodiscard]] MemoryManager& memoryManager() { return m_mm; }

    // Clears any error a previous stdlib native call left set. Op::PRINT
    // (vm.cpp) calls the equivalent of this before stringify(), because
    // stringify() can call back into stdlib code (e.g. a Map's own
    // to-string); a stale flag from an unrelated earlier call must not be
    // mistaken for one stringify() just raised.
    void clearNativeError() { m_stdlibCtx.clearError(); }

    // True if a stdlib native call set an error since the last
    // clearNativeError(). Clears the flag either way, so a caller cannot
    // observe the same error twice. Mirrors the check every native-call
    // site (callNative, and vm.cpp's own Op::PRINT) already makes on
    // m_stdlibCtx.nativeError.
    bool takeNativeError(std::string* msg) {
        if (!m_stdlibCtx.nativeError) {
            return false;
        }
        if (msg != nullptr) {
            *msg = m_stdlibCtx.nativeErrorMsg;
        }
        m_stdlibCtx.clearError();
        return true;
    }

    // Test-only trace of (chunk offset, handler depth) before each
    // dispatched instruction. Null unless a test arms it. No effect on
    // release behavior: VM::run() only appends when this is set.
    void setHandlerDepthTrace(std::vector<std::pair<int, int>>* trace) {
        m_handlerDepthTrace = trace;
    }

    // Sets the command-line arguments exposed to the program via args().
    void setArgs(std::vector<std::string> args) {
        m_stdlibCtx.args = std::move(args);
    }

    // Q1/Q3 (notes/qbe-backend.md): compiled code (backend/rt_capi.cpp)
    // writes its own frame's local/temporary slots directly at base + 8h,
    // bypassing push()'s own STACK_MAX check entirely. Without a check of
    // its own, an unbounded compiled recursion would write straight past
    // `stack[STACK_MAX + STACK_OVERFLOW_STACK_RESERVE]` — memory
    // corruption, not a reported error. `neededTop` is the highest address
    // this call's own frame can ever reach (its own analyzed max stack
    // height, from the compiler emitting this check once at function
    // entry) — mirrors call()'s own FRAMES_MAX guard exactly (same
    // handler-active/unwinding widening, same catchable-vs-fatal split),
    // so the QBE and native paths overflow at the same call depth.
    OpResult checkStackOverflow(Value* neededTop, int stopAtFrameCount) {
        std::ptrdiff_t hardCeiling =
            STACK_MAX +
            (m_unwindingStackOverflow ? STACK_OVERFLOW_STACK_RESERVE : 0);
        if (neededTop <= stack + hardCeiling) {
            return OpResult::OK;
        }
        if (!m_handlerStack.empty() && !m_unwindingStackOverflow) {
            m_unwindingStackOverflow = true;
            ThrowOutcome outcome = raiseThrowableError(
                "StackOverflowError", "Stack overflow.", stopAtFrameCount);
            m_unwindingStackOverflow = false;
            return fromThrow(outcome);
        }
        runtimeError("Stack overflow.");
        return OpResult::Fatal;
    }

  private:
    static OpResult fromThrow(ThrowOutcome outcome) {
        switch (outcome) {
        case ThrowOutcome::Uncaught:
            return OpResult::Fatal;
        case ThrowOutcome::HandledStop:
            return OpResult::Stop;
        case ThrowOutcome::HandledContinue:
        case ThrowOutcome::Pushed:
            return OpResult::Resumed;
        }
        return OpResult::Fatal; // unreachable
    }

    // opCall()'s closure branch when the callee's function->code is already
    // attached (rt_attach_code, backend/rt_capi.cpp): pushes the CallFrame
    // exactly as call() does (same arity check, same stack-overflow check,
    // same GC-visible bookkeeping), then invokes the attached code directly
    // instead of leaving the frame for VM::run() to interpret. The compiled
    // callee's own bytecode offset, defer list, and open upvalues are its
    // own business; this only owns the frame's entry and exit.
    //
    // S6 (#459): the compiled callee's own return value is now a real
    // three-way status (kRtOk/kRtThrow/kRtFatal — backend/rt_abi.h).
    // kRtFatal collapses straight to OpResult::Fatal, same as before.
    // kRtThrow means the callee's own CallFrame is already gone (unwound by
    // handleThrow as part of resolving whichever fault propagated out of
    // it) — this translates that back into the OpResult/stopAtFrameCount
    // convention the rest of Runtime already understands, exactly the way
    // fromThrow() does for every other call site: Resumed when this
    // invocation's own context (m_frameCount > stopAtFrameCount) is still
    // live, Stop otherwise.
    // The result-type contract Op::RETURN enforces for a dispatched operator
    // method (see dispatchMethod). A violated contract raises the catchable
    // OperatorResultTypeError. Declared here, ahead of callCompiled/
    // dispatchMethod below, so both can use it in their own signatures.
    enum class ResultCheck : std::uint8_t {
        None,
        Boolean,  // __eq__ __lt__ __gt__ __contains__
        Number,   // __len__
        Sequence, // __iter__ — and Op::RETURN builds an iterator from the
                  // result
        String,   // __str__
    };

    // `check`/`resultOverride` (S7, #460): when the callee is compiled,
    // this method itself performs the frame-exit step Op::RETURN performs
    // for an interpreted callee (vm.cpp) — compiled RETURN
    // (backend/qbe_emitter.cpp) only leaves the return value on the stack
    // and never sees these fields. Defaulted so invokeClosure's own call
    // (below) is unaffected: a plain call is never checked or overridden.
    OpResult callCompiled(ObjClosure* closure, int argCount,
                          int stopAtFrameCount,
                          ResultCheck check = ResultCheck::None,
                          const Value* resultOverride = nullptr);

    // Dispatches to callCompiled() when `closure` already has attached code,
    // else falls back to call()+fromThrow() — the same branch opCall()'s
    // closure case already makes inline (above). Every other site that runs
    // a resolved ObjClosure straight (init(), the instance/super fast-path
    // method call, a bound method's underlying closure) must branch the
    // same way: under the QBE backend there is no VM::run() loop left to
    // pick a bare call()'s pushed-but-not-run frame back up (OpResult::
    // Resumed's own contract above: "reload frame/ip/chunk from the
    // current top before resuming dispatch") — nothing would ever run it
    // (S5, #458). dispatchMethod() (below) makes the identical branch for
    // the operator-overloading protocol's own dispatch, passing its
    // ResultCheck/resultOverride straight through to callCompiled().
    OpResult invokeClosure(ObjClosure* closure, int argCount,
                           int stopAtFrameCount);

    // The operator-overloading protocol methods, in the order they are
    // interned into m_protocolNames. `Count` is the array size.
    enum class Protocol : std::uint8_t {
        Add,
        RAdd,
        Sub,
        RSub,
        Mul,
        RMul,
        Div,
        RDiv,
        Mod,
        RMod,
        Neg,
        Lt,
        RLt,
        Gt,
        RGt,
        Eq,
        Contains,
        Call,
        IndexGet,
        IndexSet,
        Len,
        Iter,
        Slice,
        Hash,
        Str,
        Count,
    };

    // Interns the protocol names into m_protocolNames. Called once from
    // defineNatives(), after the VM is assembled but before any bytecode can
    // run, so a dispatch helper never interns on the hot path.
    void initProtocolNames();

    // Dispatches a binary operator to `proto`'s method on the LEFT operand
    // (peek(1)), passing the RIGHT operand (peek(0)) as the single argument.
    // The left operand's class method table is the only lookup; a field named
    // like the method never participates. Returns nullopt when the left
    // operand is not an Instance that defines the method, in which case the
    // caller falls through to the opcode's own error. Otherwise returns the
    // OpResult of the method call, whose result is left on the stack.
    std::optional<OpResult> tryBinaryMethod(Protocol proto,
                                            int stopAtFrameCount);

    // tryBinaryMethod plus Boolean-result validation: the pushed frame is
    // marked so Op::RETURN validates its result as a Boolean and raises the
    // catchable OperatorResultTypeError otherwise.
    std::optional<OpResult> tryBinaryMethodBool(Protocol proto,
                                                int stopAtFrameCount);

    // The reflected form of tryBinaryMethod: dispatches `proto`'s method on
    // the RIGHT operand (peek(0)), passing the LEFT operand (peek(1)) as the
    // single argument. Called only after tryBinaryMethod declined — the left
    // operand is not an Instance that defines the method. Used for `__radd__`
    // and the other `__r*__` methods, and for the reflected `__eq__` (which
    // reuses Protocol::Eq). The two stack slots are swapped so the right
    // operand becomes the receiver (slot 0) and the left operand the argument
    // (slot 1), exactly as opIn does for `__contains__`. Returns nullopt when
    // the right operand is not an Instance that defines the method, in which
    // case the caller falls through to the opcode's own error.
    std::optional<OpResult> tryReflectedBinaryMethod(Protocol proto,
                                                     int stopAtFrameCount);

    // tryReflectedBinaryMethod plus Boolean-result validation (reflected
    // comparison and equality).
    std::optional<OpResult> tryReflectedBinaryMethodBool(Protocol proto,
                                                         int stopAtFrameCount);

    // Calls `method` — a resolved ObjClosure whose receiver already sits at
    // stackTop[-argCount-1] — and, when `check` is not None, marks the pushed
    // frame so Op::RETURN validates its result type and raises the catchable
    // OperatorResultTypeError otherwise. The method itself runs in the
    // ordinary interpreter loop; the validation happens at RETURN, not here,
    // so a throw from the method propagates normally with no re-entrant run()
    // invocation to confuse it with a caught throw. When `resultOverride` is
    // non-null, Op::RETURN discards the method's actual return and pushes
    // `*resultOverride` instead (used by __index_set__, whose assignment value
    // is the assigned value, not the method's return).
    OpResult dispatchMethod(ObjClosure* method, int argCount,
                            int stopAtFrameCount, ResultCheck check,
                            const Value* resultOverride = nullptr);

    // Sized FRAMES_MAX/STACK_MAX plus the reserve above, not just
    // FRAMES_MAX/STACK_MAX: the reserve is spent above those ceilings, while
    // a StackOverflowError unwinds (see m_unwindingStackOverflow and
    // STACK_OVERFLOW_FRAME_RESERVE/STACK_OVERFLOW_STACK_RESERVE), so the
    // physical storage must reach past them or that unwind's own pushes run
    // out of bounds.
    CallFrame m_frames[FRAMES_MAX + STACK_OVERFLOW_FRAME_RESERVE];
    int m_frameCount{0};
    Value stack[STACK_MAX + STACK_OVERFLOW_STACK_RESERVE];
    Value* stackTop;
    bool m_stackOverflow{false};
    // True for the duration of one StackOverflowError's own handleThrow()
    // call (set and cleared by two plain assignments, at both overflow
    // guards below). Draining a discarded frame's defer runs arbitrary
    // Lox++ code (runPendingDefers() -> a nested run()), and that code can
    // itself recurse deep enough to reach either overflow guard again —
    // reachable only because the reserve lets a StackOverflowError's own
    // unwind run deferred calls at all; every frame between the original
    // overflow and the handler can hold one. Without this flag, each such
    // nested hit re-enters handleThrow()'s own unwind through genuine C++
    // recursion (call() -> raiseThrowableError() -> handleThrow() ->
    // runPendingDefers() -> a nested run() -> call() -> ...), one level
    // per remaining frame, crashing the process with a real native stack
    // overflow instead of a Lox++-level fault. Both overflow guards skip the
    // catchable path while this flag is set, falling straight through to
    // their own hard ceiling (FRAMES_MAX/STACK_MAX plus the reserve) instead:
    // the second overflow stays fatal, it is never handed to the same
    // handler a second time.
    bool m_unwindingStackOverflow{false};
    MemoryManager m_mm;
    Table m_globals;
    std::array<ObjString*, static_cast<std::size_t>(Protocol::Count)>
        m_protocolNames{};
    ObjUpvalue* m_openUpvalues{nullptr};
    Value m_lastResult; // For testing/debugging only -- stores the value
                        // popped by Op::POP.
    StdlibContext m_stdlibCtx;
    ObjClass* m_fileClass{nullptr};
    ObjClass* m_mapClass{nullptr};
    ObjClass* m_errorClass{nullptr};
    ObjClass* m_socketClass{nullptr};
    ObjClass* m_serverClass{nullptr};
    ObjClass* m_processClass{nullptr};
    ObjClass* m_coroutineClass{nullptr};

    // The coroutine currently executing, or nullptr for the root coroutine.
    // A coroutine's live state sits on the shared stack while it runs; this
    // pointer is what markRoots walks to keep the object and its callee alive.
    ObjCoroutine* m_currentCoroutine{nullptr};
    // The value Op::YIELD popped, handed back to whoever resumed the
    // coroutine. Rooted in markRoots for the brief window before resume
    // copies it out.
    Value m_yieldedValue;

    // Handler stack for try/catch — parallel to m_frames[].
    // m_handlerStack[i] records {frameCount, stackTop, catchIp} for the
    // i-th PUSH_HANDLER. THROW searches LIFO for a matching handler.
    //
    // INVARIANT(handler-stack-scoped-cleanup): on every non-local control-flow
    // exit that leaves a protected region (RETURN, or break/continue from a
    // try body or from a catch block), every handler record opened since
    // that exit started is discarded via POP_HANDLER before the jump occurs.
    // A catch block holds no record of its own try: THROW already removed
    // it. On RETURN paths (with or without
    // pending defers), every record with frameCount equal to the frame being
    // left is also discarded before that frame's slot in m_frames[] is reused,
    // and before any of that frame's own defers run. This ensures no throw at
    // the same or a shallower depth — including one raised by the frame's own
    // deferred call — can match a record whose stackTop/catchIp point into a
    // frame and a chunk that no longer exist, or into a protected region the
    // jump already left.
    std::vector<HandlerRecord> m_handlerStack;

    // Backing store for setHandlerDepthTrace(). Never read by Runtime
    // itself; the test harness owns the pointed-to vector.
    std::vector<std::pair<int, int>>* m_handlerDepthTrace{nullptr};

    // Per-frame defer lists — parallel to m_frames[], so sized to match it
    // (FRAMES_MAX plus the reserve; see m_frames' own comment). Each entry is
    // a vector of ObjClosure* (thunks) pending invocation LIFO.
    std::array<std::vector<Value>, FRAMES_MAX + STACK_OVERFLOW_FRAME_RESERVE>
        m_deferLists;

    // Parallel to m_frames[]: the result-type contract Op::RETURN enforces
    // for this frame's operator-overloading method (None for ordinary frames).
    // Set by dispatchMethod(), cleared by RETURN, callCompiled(),
    // handleThrow()'s unwind loop, and resetStack(). Sized to match m_frames.
    std::array<ResultCheck, FRAMES_MAX + STACK_OVERFLOW_FRAME_RESERVE>
        m_frameResultCheck{};

    // Parallel to m_frames[]: when m_frameResultOverrideSet[i] is true,
    // Op::RETURN pushes m_frameResultOverride[i] instead of the method's
    // actual return (__index_set__'s assignment value). Same set/clear sites
    // as m_frameResultCheck.
    std::array<bool, FRAMES_MAX + STACK_OVERFLOW_FRAME_RESERVE>
        m_frameResultOverrideSet{};
    std::array<Value, FRAMES_MAX + STACK_OVERFLOW_FRAME_RESERVE>
        m_frameResultOverride{};

    // See setInterpretLoop() above.
    std::function<InterpretResult(int)> m_runLoop;

    // Set by invokeCallableFromNative() when a native's re-entrant call did
    // not return a value (Stop/Fatal/Resumed). callNative() reads it after the
    // native returns and propagates it instead of pushing the native's
    // placeholder result. Reset to OK before each native call.
    OpResult m_reentrantOutcome{OpResult::OK};

    // Set by hashMapKey() or mapKeyEq() when a key operation's __hash__/__eq__
    // fails: Stop/Fatal to propagate, or Resumed if a caught error continued
    // inside the enclosing run(). Reset to OK before each map key operation.
    OpResult m_mapKeyStatus{OpResult::OK};

    // The stopAtFrameCount of the run() invocation that called the native
    // currently executing. Saved/restored around each callNative() so a native
    // re-entering the VM judges a caught throw against the right boundary.
    int m_nativeStopAtFrameCount{0};

    // Number of C++ re-entrant runs currently on the native stack — every
    // runReentrantFrame() invocation. A Lox frame inside one of these cannot
    // be captured by a coroutine snapshot: its C++ continuation would be
    // lost. Op::YIELD raises YieldAcrossNativeError while this is nonzero.
    int m_reentrantRunDepth{0};

    // The enclosing run()'s boundary for the canonical stringify in progress,
    // and the outcome of a __str__ dispatch inside it. stringifyObj
    // (object.cpp) reaches __str__ through stringifyInstanceStr() below, so it
    // never sees the boundary itself: opStr() (print/str) and handleThrow()
    // (the report of an uncaught non-Error throw) both set it, and each
    // saves/restores it around its own stringify.
    int m_stringifyBoundary{0};
    OpResult m_stringifyStatus{OpResult::OK};
    // Non-zero while a canonical stringify is in progress (print/str, or the
    // report of an uncaught non-Error throw): stringifyObj's INSTANCE case
    // dispatches __str__ only then, so an internal stringify (the debug
    // trace's stack dump, the disassembler) still renders the default form
    // with no user-code side effect.
    int m_stringifyCanonicalDepth{0};

#ifdef LOXPP_PROFILE
    ProfilerData m_profilerData;
    // The profiler the dispatch loop currently attributes work to: the root's
    // inline m_profilerData, or a running coroutine's own ProfilerData.
    // resumeCoroutine swaps this; suspendCurrentCoroutine and a normal return
    // leave the coroutine's scope slice in the coroutine. The report reads
    // only the root; merging the per-coroutine tables is issue #538.
    ProfilerData* m_activeProfiler{&m_profilerData};
    // Parallel to m_frames[]: active ProfileFunctionScope per call depth.
    // .emplace() at function entry; .reset() at Op::RETURN. Sized to match
    // m_frames (FRAMES_MAX plus the reserve; see its own comment).
    std::array<std::optional<ProfileFunctionScope>,
               FRAMES_MAX + STACK_OVERFLOW_FRAME_RESERVE>
        m_profilerScopes;

  public:
    const ProfilerData& profilerData() const { return m_profilerData; }

  private:
#endif
};

#ifdef LOXPP_PROFILE
// frameEnterNs runs parallel to m_frames[]; its size must track any change
// to the frame budget or the overflow reserve.
static_assert(std::tuple_size_v<decltype(ProfilerData::frameEnterNs)> ==
                  Runtime::FRAMES_MAX + Runtime::STACK_OVERFLOW_FRAME_RESERVE,
              "frameEnterNs size must match Runtime's m_frames capacity");
#endif
