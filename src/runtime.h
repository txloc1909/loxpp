#pragma once

// Runtime — the VM's state, and every operation on that state that does not
// need the interpreter dispatch loop's own register-cached ip/frame/chunk
// locals. VM::run() (vm.h/vm.cpp) is the interpreter loop: it stays a
// separate class because caching ip/frame/chunk in local variables (rather
// than re-reading them from Runtime on every instruction) is what makes the
// dispatch loop fast, and only the loop itself needs them.
//
// Compiled Lox++ code calls the entry points this class exposes —
// push/pop/peek, call(), and the op*() opcode helpers below — through a
// C wrapper, instead of going through VM::run()'s switch. Nothing in this
// class assumes an interpreter is driving it.
//
// A representative slice of opcode bodies (the ones with the most runtime
// polymorphism: CALL/INVOKE/SUPER_INVOKE dispatch, GET_PROPERTY/GET_SUPER,
// INHERIT, GET_INDEX/SET_INDEX, and the iterator ops) has moved into the
// op*() methods below — Layer 1 of the same proposal. The rest (arithmetic,
// locals/globals, control flow, RETURN, defer/handler bookkeeping) is still
// inline in VM::run(), reached through the friendship grant below; the
// design doc's Layer 1 section marks that remaining work.
//
// VM::run() needs raw access to this class's own bookkeeping (m_frames,
// stack/stackTop, m_handlerStack, ...) for the opcodes that have not moved
// out yet, so VM is a friend rather than going through a pile of one-off
// accessors that would only exist to satisfy that one caller. The op*()
// methods and the other operations below are public because QBE's C
// wrappers (a later stage) are not friends — they can only reach this class
// through its public surface.

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

    // Outcome of a throw/catch dispatch (handleThrow/raiseThrowableError).
    // stopAtFrameCount always names the boundary of whichever run()
    // invocation is currently, actually dispatching — the run() parameter
    // itself for Op::THROW and the tryCatchableError lambda, and the same
    // value threaded down through runPendingDefers/call() for a reentrant
    // fault inside a running defer. Uncaught: no handler found, runtimeError()
    // already called — caller returns InterpretResult::RUNTIME_ERROR.
    // HandledContinue: a handler was found (here, or by a reentrant call
    // several levels down) and the resulting m_frameCount is still above the
    // caller's own stopAtFrameCount — the caller's own frame context is still
    // live; it must FrameSync::loadTop (or let its own ambient FrameSync
    // guard do so) and continue dispatch normally. HandledStop: handled, but
    // the resulting m_frameCount is at or below the caller's own
    // stopAtFrameCount — control now belongs to a different, less-nested
    // run() invocation. The caller must NOT read frame/ip/chunk (m_frameCount
    // may even be 0, making that read out of bounds) and must return
    // InterpretResult::OK immediately.
    enum class ThrowOutcome : std::uint8_t {
        Uncaught,
        HandledContinue,
        HandledStop
    };

    // Outcome of call(). Pushed: a new frame is on top of m_frames; proceed
    // normally. CaughtContinue/CaughtStop: no new frame was pushed — an
    // arity mismatch was itself caught (see ThrowOutcome above for what the
    // two names mean) — the caller must react exactly as it would to the
    // matching ThrowOutcome. Uncaught: hard, already-reported error — caller
    // returns InterpretResult::RUNTIME_ERROR.
    enum class CallOutcome : std::uint8_t {
        Pushed,
        CaughtContinue,
        CaughtStop,
        Uncaught,
    };

    // Outcome of an op*() opcode helper below. These helpers can allocate,
    // call (pushing a new CallFrame), and throw (unwinding zero or more
    // frames) — Continue, Stop and Fatal collapse ThrowOutcome and
    // CallOutcome above into the three shapes a caller actually has to
    // react to differently:
    //   OK:    everything the helper did (if anything) is committed to
    //          m_frames/m_frameCount already; the caller reloads frame/ip/
    //          chunk from the current top of m_frames before resuming
    //          dispatch. Covers plain success, a caught error resolved
    //          within the caller's own run() invocation, and a completed
    //          call push — the reload is a correct no-op in the first case
    //          and load-bearing in the other two.
    //   Stop:  resolved by a handler outside the caller's own run()
    //          invocation (ThrowOutcome::HandledStop/CallOutcome::
    //          CaughtStop). Caller returns InterpretResult::OK immediately
    //          without touching frame/ip/chunk.
    //   Fatal: uncaught error already reported via runtimeError(). Caller
    //          returns InterpretResult::RUNTIME_ERROR immediately.
    enum class OpResult : std::uint8_t { OK, Stop, Fatal };

    // White-box seam for StackOverflowTest and VM's own interpreter loop.
    friend class VM;
    friend struct VMTestAccess;

    void resetStack();

    // Defined inline (not in runtime.cpp): these are called on every single
    // opcode dispatch, and vm.cpp is a separate translation unit from
    // runtime.cpp with no LTO — an out-of-line definition here measured
    // roughly 4x slower on a call-heavy benchmark (fib(32), release preset)
    // because the compiler could no longer inline them into VM::run()'s
    // loop. Keep the operand-stack primitives header-only so any TU that
    // includes runtime.h — including QBE's future C wrappers — gets the
    // same inlining that the single-file VM used to get for free.
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

    CallOutcome call(ObjClosure* closure, int argCount,
                     int stopAtFrameCount = 0);
    bool callNative(ObjNative* native, int argCount);
    bool callBoundNative(ObjBoundNative* bn, int argCount);
    bool bindMethod(ObjClass* klass, ObjString* name);
    void defineNatives();
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

    // --- op*() opcode helpers (Layer 1) -------------------------------
    //
    // Each one has the stack effect of the opcode it implements and takes,
    // beyond `this`, only the operands that opcode already decoded from the
    // bytecode stream (a constant-pool name, an argument count) — never
    // ip/chunk. The interpreter loop decodes those operands (it has ip/
    // chunk), flushes the current frame's ip so a runtimeError() raised in
    // here reports the right line, and then calls straight through. A QBE
    // wrapper does the same: read its own copy of the operand out of the
    // compiled constant, store the bytecode offset into its frame slot
    // (Q4 in notes/qbe-backend.md), and call the same function.
    OpResult opCall(int argCount, int stopAtFrameCount);
    OpResult opInvoke(ObjString* name, int argCount, int stopAtFrameCount);
    OpResult opGetProperty(ObjString* name, int stopAtFrameCount);
    OpResult opGetSuper(ObjString* name);
    OpResult opSuperInvoke(ObjString* name, int argCount, int stopAtFrameCount);
    OpResult opInherit();
    OpResult opGetIndex(int stopAtFrameCount);
    OpResult opSetIndex(int stopAtFrameCount);
    OpResult opGetIter();
    OpResult opIterHasNext();
    OpResult opIterNext();

    // Runtime state inspection (for testing and debugging).
    [[nodiscard]] int stackDepth() const {
        return static_cast<int>(stackTop - stack);
    }
    [[nodiscard]] int frameCount() const { return m_frameCount; }
    [[nodiscard]] int handlerStackDepth() const {
        return static_cast<int>(m_handlerStack.size());
    }
    [[nodiscard]] std::optional<Value> getGlobal(const std::string& name) const;
    [[nodiscard]] Value lastResult() const { return m_lastResult; }

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

  private:
    static OpResult fromThrow(ThrowOutcome outcome) {
        switch (outcome) {
        case ThrowOutcome::Uncaught:
            return OpResult::Fatal;
        case ThrowOutcome::HandledStop:
            return OpResult::Stop;
        case ThrowOutcome::HandledContinue:
            return OpResult::OK;
        }
        return OpResult::Fatal; // unreachable
    }
    static OpResult fromCall(CallOutcome outcome) {
        switch (outcome) {
        case CallOutcome::Pushed:
        case CallOutcome::CaughtContinue:
            return OpResult::OK;
        case CallOutcome::CaughtStop:
            return OpResult::Stop;
        case CallOutcome::Uncaught:
            return OpResult::Fatal;
        }
        return OpResult::Fatal; // unreachable
    }

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
    ObjUpvalue* m_openUpvalues{nullptr};
    Value m_lastResult; // For testing/debugging only -- stores the value
                        // popped by Op::POP.
    StdlibContext m_stdlibCtx;
    ObjClass* m_fileClass{nullptr};
    ObjClass* m_mapClass{nullptr};
    ObjClass* m_errorClass{nullptr};

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

    // See setInterpretLoop() above.
    std::function<InterpretResult(int)> m_runLoop;

#ifdef LOXPP_PROFILE
    ProfilerData m_profilerData;
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
