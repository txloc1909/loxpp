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
    static constexpr int STACK_MAX = 2048;
    static constexpr int FRAMES_MAX = 256;

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
    // INVARIANT(handler-stack-frame-scoped): every record's frameCount is
    // the depth (m_frameCount) of the call frame whose still-open protected
    // region pushed it. Any path that leaves that frame — not only THROW's
    // own unwind, but also a plain RETURN taken before the region's
    // POP_HANDLER runs — must discard every record with that frameCount
    // before the frame's slot in m_frames[] can be reused. Otherwise a
    // later throw at the same or a shallower depth can match a record whose
    // stackTop/catchIp point into a frame and a chunk that no longer exist.
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
