#pragma once

#include "exec_objects.h"
#include "class_objects.h"
#include "memory_manager.h"
#include "table.h"
#include "stdlib/stdlib_context.h"

#include <cstdint>

#ifdef LOXPP_PROFILE
#include "profiler.h"
#endif

#include <memory>
#include <optional>
#include <string>
#include <vector>

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

class VM {
  public:
    static constexpr int STACK_MAX = 16384;
    static constexpr int FRAMES_MAX = 1024;

    // Both overflow guards (the frame-count check in call(), the value-stack
    // check in push()) fire this many units before their hard ceiling above,
    // once a handler is active, so raising StackOverflowError as a catchable
    // Error still leaves room to unwind: closing upvalues, draining each
    // discarded frame's pending defers (each one a real, if short-lived,
    // nested call() — see handleThrow()/runPendingDefers()), and allocating
    // the Error object. With no handler active neither guard reserves
    // anything — a plain, uncaught overflow still uses every frame/slot up
    // to the hard ceiling, unchanged from before this reserve existed.
    // Measured directly against this reserve (test_vm_runtime.cpp's
    // StackOverflowTest suite): one deferred call with a short (non-
    // recursive) body needs one CallFrame and a handful of value-stack
    // slots; a chain of ordinary calls inside that body costs more of each.
    // A deferred call that outruns this reserve hits the hard ceiling
    // instead — fatal, or caught by whatever handler still has room, per
    // handleThrow()'s existing generic unwind — never a hang, since the
    // reserve is small enough that no path through it can recurse for long.
    static constexpr int STACK_OVERFLOW_FRAME_RESERVE = 16;
    static constexpr int STACK_OVERFLOW_STACK_RESERVE = 64;

    VM() : m_globals(VmAllocator<Entry>{&m_mm}) {
        resetStack();
        m_mm.setMarkRootsCallback([this]() { markRoots(); });
#ifdef LOXPP_PROFILE
        m_mm.setProfilerData(&m_profilerData);
#endif
    }

    InterpretResult interpret(const std::string& source);
    // stopAtFrameCount: returns InterpretResult::OK as soon as m_frameCount
    // drops to or below this value, instead of only at program exit
    // (frameCount == 0). Lets a deferred call run to completion via a
    // nested run() invocation — see runPendingDefers() — before its
    // recorder resumes. The top-level call (stopAtFrameCount == 0, the
    // default) is unaffected: m_frameCount never reaches 0 except at
    // program exit, which the frameCount == 0 branch already handles.
    InterpretResult run(int stopAtFrameCount = 0);
    [[nodiscard]] Value lastResult() const;

    // Runtime state inspection (for testing and debugging).
    [[nodiscard]] int stackDepth() const {
        return static_cast<int>(stackTop - stack);
    }
    [[nodiscard]] int frameCount() const { return m_frameCount; }
    [[nodiscard]] int handlerStackDepth() const {
        return static_cast<int>(m_handlerStack.size());
    }
    [[nodiscard]] std::optional<Value> getGlobal(const std::string& name) const;

    // Sets the command-line arguments exposed to the program via args().
    void setArgs(std::vector<std::string> args) {
        m_stdlibCtx.args = std::move(args);
    }

  private:
    void resetStack();
    void push(Value value);
    Value pop();
    Value peek(int distance);

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

    // Outcome of VM::call(). Pushed: a new frame is on top of m_frames;
    // proceed normally. CaughtContinue/CaughtStop: no new frame was pushed —
    // an arity mismatch was itself caught (see ThrowOutcome above for what
    // the two names mean) — the caller must react exactly as it would to the
    // matching ThrowOutcome. Uncaught: hard, already-reported error — caller
    // returns InterpretResult::RUNTIME_ERROR.
    enum class CallOutcome : std::uint8_t {
        Pushed,
        CaughtContinue,
        CaughtStop,
        Uncaught,
    };

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

    CallFrame m_frames[FRAMES_MAX];
    int m_frameCount{0};
    Value stack[STACK_MAX];
    Value* stackTop;
    bool m_stackOverflow{false};
    // True for the duration of one StackOverflowError's own handleThrow()
    // call (set and cleared by two plain assignments, at both overflow
    // guards in vm.cpp). Draining a discarded frame's defer runs arbitrary
    // Lox++ code (runPendingDefers() -> a nested run()), and that code can
    // itself recurse deep enough to reach either overflow guard again —
    // reachable only because the reserve now lets a StackOverflowError's own
    // unwind run deferred calls at all; every frame between the original
    // overflow and the handler can hold one. Without this flag, each such
    // nested hit re-enters handleThrow()'s own unwind through genuine C++
    // recursion (VM::call() -> raiseThrowableError() -> handleThrow() ->
    // runPendingDefers() -> a nested run() -> VM::call() -> ...), one level
    // per remaining frame, and measurably crashes the process with a real
    // native stack overflow — verified directly: with this flag's guard
    // condition removed from both call sites and an ASan debug build,
    // test_vm_runtime's SecondOverflowDuringUnwindDoesNotHangOrCorrupt
    // aborts with "AddressSanitizer: stack-overflow", not a clean test
    // failure. (An earlier build of this guard used `==` instead of `>=`
    // against the frame-count threshold below; that unrelated bug capped
    // the nested recursion at one extra level and hid this crash — fixed
    // together with this comment, not evidence the guard is unneeded.) Both
    // overflow guards skip the catchable path while this flag is set,
    // falling straight through to their own hard ceiling instead: the
    // second overflow "stays fatal", per this row's own original design
    // note.
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
    // exit that leaves a protected region (RETURN, break, or continue), every
    // handler record opened since that exit started is discarded via
    // POP_HANDLER before the jump occurs. On RETURN paths (with or without
    // pending defers), every record with frameCount equal to the frame being
    // left is also discarded before that frame's slot in m_frames[] is reused,
    // and before any of that frame's own defers run. This ensures no throw at
    // the same or a shallower depth — including one raised by the frame's own
    // deferred call — can match a record whose stackTop/catchIp point into a
    // frame and a chunk that no longer exist, or into a protected region the
    // jump already left.
    std::vector<HandlerRecord> m_handlerStack;

    // Per-frame defer lists — parallel to m_frames[]. Each entry is a
    // vector of ObjClosure* (thunks) pending invocation LIFO.
    std::array<std::vector<Value>, FRAMES_MAX> m_deferLists;

#ifdef LOXPP_PROFILE
    ProfilerData m_profilerData;
    // Parallel to m_frames[]: active ProfileFunctionScope per call depth.
    // .emplace() at function entry; .reset() at Op::RETURN.
    std::array<std::optional<ProfileFunctionScope>, FRAMES_MAX>
        m_profilerScopes;

  public:
    const ProfilerData& profilerData() const { return m_profilerData; }

  private:
#endif
};
