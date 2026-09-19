#include "vm.h"
#include "debug.h"
#include "objects.h"
#include "memory_manager.h"
#include "object.h"
#include "scanner.h"
#include "compiler.h"
#include "utility.h"

#include "stdlib/stdlib_context.h"
#include "stdlib/stdlib_registrar.h"
#include "stdlib/globals.h"
#include "stdlib/file_api.h"
#include "stdlib/map_api.h"
#include "stdlib/error_api.h"
#include "stdlib/math_module.h"
#include "stdlib/os_api.h"
#include "stdlib/reflect_api.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <unistd.h>

InterpretResult VM::interpret(const std::string& source) {
    // Guard against dangling class pointers from a prior VM instance. GC can
    // fire inside compile(), and markRoots() must not dereference a pointer
    // that was freed when the previous VM's MemoryManager was destroyed.
    m_fileClass = nullptr;
    m_mapClass = nullptr;
    ObjFunction* fn = compile(source, &m_mm);
    if (fn == nullptr) {
        return InterpretResult::COMPILE_ERROR;
    }

    // Root fn on the stack before any allocation (defineNatives,
    // create<ObjClosure>) can trigger GC. Without this, fn is unreachable
    // between compile() returning and push(closure) — the Compiler has already
    // been destroyed and m_currentCompiler is nullptr.
    push(Value{static_cast<Obj*>(fn)});
    m_stdlibCtx.mm = &m_mm;
    setActiveContext(&m_stdlibCtx);
    defineNatives();
    ObjClosure* closure = m_mm.create<ObjClosure>(fn);
    stackTop[-1] = Value{
        static_cast<Obj*>(closure)}; // replace fn with its closure in-place
    // No handler can be active yet (nothing has executed), so the only
    // reachable outcome here is Pushed or Uncaught.
    if (call(closure, 0) == CallOutcome::Uncaught) {
        return InterpretResult::RUNTIME_ERROR;
    }
#ifdef LOXPP_PROFILE
    // Count the implicit script call so Op::CALL and Op::RETURN stay balanced.
    m_profilerData.opcodeTable[static_cast<uint8_t>(Op::CALL)].count++;
    ProfileProgramScope programScope(m_profilerData);
#endif
    return run();
}

Value VM::lastResult() const { return m_lastResult; }

std::optional<Value> VM::getGlobal(const std::string& name) const {
    ObjString* key = m_mm.findString(name);
    if (!key) {
        return std::nullopt;
    }
    Value out;
    if (!m_globals.get(key, out)) {
        return std::nullopt;
    }
    return out;
}

VM::CallOutcome VM::call(ObjClosure* closure, int argCount,
                         int stopAtFrameCount) {
    ObjFunction* fn = closure->function;
    if (argCount != fn->arity) {
        // Arity mismatch is now catchable as ArityError, but only if a handler
        // is active. If no handler is active, fall back to uncaught error.
        // The arity check happens before any new frame is pushed, so it never
        // needs the frame reserve StackOverflowError's own guard below relies
        // on.
        char msg[256];
        snprintf(msg, sizeof(msg), "Expected %d arguments but got %d.",
                 fn->arity, argCount);
        if (!m_handlerStack.empty()) {
            // stopAtFrameCount must be the boundary of whichever run()
            // invocation is actually calling us (threaded down from there,
            // not always 0) — see ThrowOutcome's doc comment in vm.h. Using
            // the wrong boundary here is exactly the class of bug that let a
            // caught fault corrupt an unrelated, more-nested run()
            // invocation's own frame bookkeeping.
            ThrowOutcome outcome =
                raiseThrowableError("ArityError", msg, stopAtFrameCount);
            if (outcome == ThrowOutcome::HandledContinue) {
                return CallOutcome::CaughtContinue;
            }
            if (outcome == ThrowOutcome::HandledStop) {
                return CallOutcome::CaughtStop;
            }
        }
        // No handler found, or error class not ready; uncaught error.
        runtimeError("Expected %d arguments but got %d.", fn->arity, argCount);
        return CallOutcome::Uncaught;
    }
    // Fires at FRAMES_MAX itself — the same threshold whether or not a
    // handler is active — so an open try/catch never changes how deep a
    // program that does not overflow can go (see
    // STACK_OVERFLOW_FRAME_RESERVE's own comment in vm.h). Only when a
    // handler is active does hitting it raise a catchable StackOverflowError
    // instead of going straight to the hard ceiling below: that unwind
    // (closing upvalues, draining each discarded frame's own defers,
    // building the Error) spends the reserve capacity held above FRAMES_MAX.
    // This must be `>=`, not `==`: m_frameCount only increases, so a try
    // opened after it has already passed FRAMES_MAX would never see an exact
    // match again.
    if (m_frameCount >= FRAMES_MAX && !m_handlerStack.empty() &&
        !m_unwindingStackOverflow) {
        // See m_unwindingStackOverflow's own comment (vm.h): a deferred call
        // drained by the handleThrow() below can itself reach this same
        // guard again. Hold the flag for exactly that call, so a nested hit
        // falls through to the hard ceiling below instead of recursing.
        m_unwindingStackOverflow = true;
        ThrowOutcome outcome = raiseThrowableError(
            "StackOverflowError", "Stack overflow.", stopAtFrameCount);
        m_unwindingStackOverflow = false;
        if (outcome == ThrowOutcome::HandledContinue) {
            return CallOutcome::CaughtContinue;
        }
        if (outcome == ThrowOutcome::HandledStop) {
            return CallOutcome::CaughtStop;
        }
        // Uncaught: raiseThrowableError() already reported it. m_frameCount
        // is reset to 0 by then (see runtimeError()/resetStack()), so
        // continuing to push a frame below would be reading a torn-down VM.
        return CallOutcome::Uncaught;
    }
    // No handler active: the true hard ceiling, unaffected by the reserve,
    // same depth as before the reserve existed. While unwinding a
    // StackOverflowError (m_unwindingStackOverflow), the ceiling moves out to
    // FRAMES_MAX + STACK_OVERFLOW_FRAME_RESERVE — the physical capacity
    // m_frames[] actually has — so a deferred call drained during that
    // unwind can use the reserve; past that, it stays fatal rather than
    // recursing into the same handler again.
    if (m_frameCount >=
        FRAMES_MAX +
            (m_unwindingStackOverflow ? STACK_OVERFLOW_FRAME_RESERVE : 0)) {
        runtimeError("Stack overflow.");
        return CallOutcome::Uncaught;
    }
    CallFrame* frame = &m_frames[m_frameCount++];
    frame->closure = closure;
    frame->ip = fn->chunk.cbegin();
    frame->slots = stackTop - argCount - 1;
#ifdef LOXPP_PROFILE
    {
        int depth = m_frameCount - 1;
        ObjClosure* parent =
            (depth > 0) ? m_frames[depth - 1].closure : nullptr;
        m_profilerScopes[depth].emplace(m_profilerData, closure, depth, parent);
    }
#endif
    return CallOutcome::Pushed;
}

ObjUpvalue* VM::captureUpvalue(Value* local) {
    ObjUpvalue* prev = nullptr;
    ObjUpvalue* cur = m_openUpvalues;
    while (cur != nullptr && cur->location > local) {
        prev = cur;
        cur = cur->next;
    }
    if (cur != nullptr && cur->location == local) {
        return cur;
    }
    ObjUpvalue* uv = m_mm.create<ObjUpvalue>(local);
    uv->next = cur;
    if (prev == nullptr) {
        m_openUpvalues = uv;
    } else {
        prev->next = uv;
    }
    return uv;
}

void VM::closeUpvalues(Value* last) {
    while (m_openUpvalues != nullptr && m_openUpvalues->location >= last) {
        ObjUpvalue* uv = m_openUpvalues;
        uv->closed = *uv->location;
        uv->location = &uv->closed;
        m_openUpvalues = uv->next;
    }
}

void VM::popHandlersOwnedByCurrentFrame() {
    // m_frameCount is still the depth of the frame being left here — see
    // INVARIANT(handler-stack-frame-scoped) on m_handlerStack's declaration
    // (vm.h). Nested protected regions opened by this same frame all share
    // that depth, so the loop clears every one of them and stops at the
    // first record belonging to an ancestor frame.
    while (!m_handlerStack.empty() &&
           m_handlerStack.back().frameCount == m_frameCount) {
        m_handlerStack.pop_back();
    }
}

InterpretResult VM::runPendingDefers(int frameIndex, int stopAtFrameCount) {
    // Run every deferred call for m_frames[frameIndex], LIFO (most recently
    // recorded first). Each one runs to completion — via a nested run() that
    // stops once m_frameCount returns to frameIndex + 1 — before the next
    // one starts: popping every frame up front and letting the ordinary
    // dispatch loop replay them would run them in declaration order instead
    // of LIFO, and would leave call()'s later pushes silently discarding the
    // stack effects of earlier ones.
    auto& deferList = m_deferLists[frameIndex];
    while (!deferList.empty()) {
        Value deferValue = deferList.back();
        deferList.pop_back();
        if (!isDeferredCall(deferValue)) {
            continue;
        }
        ObjDeferredCall* deferred = asObjDeferredCall(as<Obj*>(deferValue));
        // call()'s own convention (and DEFER_RECORD's documented "stack
        // before" shape, chunk.h) is [callee, arg0, ..., argN-1] — the
        // callee goes first, underneath its arguments, not last.
        Value calleeVal = deferred->callable;
        push(calleeVal);
        for (const Value& arg : deferred->args) {
            push(arg);
        }
        int argCount = static_cast<int>(deferred->args.size());

        // Handle various callable types (similar to Op::CALL dispatch).
        // For BoundMethod, replace the method on the stack with the receiver,
        // then call the underlying method closure.
        CallOutcome outcome;
        if (isBoundMethod(calleeVal)) {
            ObjBoundMethod* bound = asObjBoundMethod(as<Obj*>(calleeVal));
            stackTop[-argCount - 1] = bound->receiver;
            outcome = call(bound->method, argCount, stopAtFrameCount);
        } else if (isClosure(calleeVal)) {
            ObjClosure* closure = asObjClosure(as<Obj*>(calleeVal));
            outcome = call(closure, argCount, stopAtFrameCount);
        } else if (isNative(calleeVal)) {
            ObjNative* native = asObjNative(as<Obj*>(calleeVal));
            outcome = callNative(native, argCount) ? CallOutcome::Pushed
                                                   : CallOutcome::Uncaught;
        } else if (isBoundNative(calleeVal)) {
            ObjBoundNative* bound = asObjBoundNative(as<Obj*>(calleeVal));
            outcome = callBoundNative(bound, argCount) ? CallOutcome::Pushed
                                                       : CallOutcome::Uncaught;
        } else {
            // Unexpected callable type in deferred call
            runtimeError("Deferred callable has unexpected type.");
            return InterpretResult::RUNTIME_ERROR;
        }

        if (outcome == CallOutcome::Uncaught) {
            return InterpretResult::RUNTIME_ERROR;
        }
        if (outcome != CallOutcome::Pushed) {
            // The deferred call's own arity mismatch was caught instead of
            // pushing a new frame. Any handler reachable here was pushed
            // before frameIndex's own function was even called (a handler
            // scoped inside that function's body is already popped by the
            // time RUN_DEFERS runs), so frameIndex no longer exists — there
            // is no new frame to run to completion. Per defer step 5's
            // documented limitation, abandon any remaining sibling defers
            // rather than still running them.
            return InterpretResult::OK;
        }
        InterpretResult result = run(frameIndex + 1);
        if (result != InterpretResult::OK) {
            return result;
        }
        if (m_frameCount != frameIndex + 1) {
            // The deferred call's own throw propagated past this frame (it
            // no longer exists — an outer handler or program exit already
            // took over dispatch). Abandon the rest of this list; the
            // caller must notice m_frameCount changed and stop too.
            return InterpretResult::OK;
        }
    }
    return InterpretResult::OK;
}

VM::ThrowOutcome VM::handleThrow(Value thrownValue, int stopAtFrameCount) {
    // Unwind frame-by-frame, running each frame's pending defers, regardless
    // of whether a handler will ultimately be found. This unifies the search
    // and the defer-draining (spec/04-semantics.md throw Statement step 5,
    // defer Statement step 4). This is the ONE unwind implementation for both
    // explicit throw (Op::THROW) and runtime faults (IndexOutOfBoundsError,
    // etc.). See ThrowOutcome's doc comment in vm.h for what each of the
    // three results means and obligates the caller to do.

    // handleThrow() owns thrownValue for as long as it runs. markRoots()
    // marks only [stack, stackTop), and Step 2 below moves stackTop to each
    // discarded frame's own base *before* running that frame's defers, so a
    // defer that allocates (a nested, real call — runPendingDefers() ->
    // VM::call() -> VM::run()) can trigger a collection while thrownValue is
    // off the stack entirely. Root it here, once, for both callers
    // (Op::THROW and raiseThrowableError()), rather than at each call site.
    struct ThrownValueGuard {
        MemoryManager& mm;
        bool rooted;
        ThrownValueGuard(MemoryManager& mm, Value v)
            : mm(mm), rooted(isObj(v)) {
            if (rooted) {
                mm.pushTempRoot(asObj(v));
            }
        }
        ~ThrownValueGuard() {
            if (rooted) {
                mm.popTempRoot();
            }
        }
    } thrownValueGuard(m_mm, thrownValue);

    // Step 1: Find if any live handler exists (without popping it yet).
    bool foundHandler = false;
    HandlerRecord handlerToUse;
    int handlerIndex = -1;
    for (int i = (int)m_handlerStack.size() - 1; i >= 0; i--) {
        HandlerRecord handler = m_handlerStack[i];
        if (handler.frameCount <= m_frameCount) {
            // Found a live handler (innermost one, since we iterate LIFO).
            foundHandler = true;
            handlerToUse = handler;
            handlerIndex = i;
            break;
        }
    }

    // Step 2: Unwind frame-by-frame, draining defers, to either the
    // handler's frame (if found) or frame 0 (if not found).
    int targetFrameCount = foundHandler ? handlerToUse.frameCount : 0;
    while (m_frameCount > targetFrameCount) {
        int unwoundFrameIndex = m_frameCount - 1;
        closeUpvalues(m_frames[unwoundFrameIndex].slots);
        // Reclaim this frame's own window before running its defers, not
        // only at the end of the whole unwind (step 3 below). A deferred
        // call's own args are already captured on ObjDeferredCall, not read
        // off this frame's live slots, so nothing here needs them once
        // closeUpvalues has run. Without this, an unwind through many frames
        // (each with its own pending defer) leaves every one of those
        // frames' operands live on the value stack at once while the defers
        // run, so the STACK_OVERFLOW_STACK_RESERVE reserve — sized for one
        // frame's own defer call — is exhausted by the second or third
        // frame instead of lasting the whole unwind.
        stackTop = m_frames[unwoundFrameIndex].slots;
        // stopAtFrameCount is OUR OWN parameter, not unwoundFrameIndex: it
        // is the boundary of whichever run() invocation is unwinding right
        // now (see vm.h), and a reentrant fault inside this defer must be
        // judged against that same boundary, not a fresh default.
        InterpretResult result =
            runPendingDefers(unwoundFrameIndex, stopAtFrameCount);
        if (result != InterpretResult::OK) {
            // Hard error during defer, already reported.
            return ThrowOutcome::Uncaught;
        }
        if (m_frameCount != unwoundFrameIndex + 1) {
            // Deferred call threw (or its own arity mismatch was caught);
            // that inner dispatch already fully resolved things — possibly by
            // running the rest of the program to completion, which can leave
            // m_frameCount at 0. Our own unwind has nothing left to finish:
            // whether OUR caller must also stop depends on where m_frameCount
            // landed relative to OUR OWN stopAtFrameCount.
            return (m_frameCount <= stopAtFrameCount)
                       ? ThrowOutcome::HandledStop
                       : ThrowOutcome::HandledContinue;
        }
#ifdef LOXPP_PROFILE
        m_profilerScopes[m_frameCount - 1].reset();
#endif
        m_frameCount--;
    }

    // Step 3: After unwinding is complete, decide what to do.
    if (foundHandler) {
        // Truncate stack to checkpoint and push thrown value.
        stackTop = handlerToUse.stackTop;
        push(thrownValue);
        // Set IP to catch block in the frame record directly.
        m_frames[m_frameCount - 1].ip = handlerToUse.catchIp;
        // Pop this handler since we're handling the throw.
        m_handlerStack.erase(m_handlerStack.begin() + handlerIndex);
        return (m_frameCount <= stopAtFrameCount)
                   ? ThrowOutcome::HandledStop
                   : ThrowOutcome::HandledContinue;
    }

    // No handler found — report uncaught error (after defers have run).
    if (isError(thrownValue)) {
        ObjError* err = asObjError(as<Obj*>(thrownValue));
        runtimeError("%s", err->message->chars.c_str());
    } else {
        std::string thrownStr = stringify(thrownValue);
        runtimeError("Uncaught throw: %s", thrownStr.c_str());
    }
    return ThrowOutcome::Uncaught;
}

VM::ThrowOutcome VM::raiseThrowableError(const char* kind_str, const char* msg,
                                         int stopAtFrameCount) {
    // Shared implementation for raising a catchable runtime error. Used by both
    // run()'s tryCatchableError lambda and by call()'s arity check. Constructs
    // an Error instance with the given kind and message, then calls handleThrow
    // to search for a handler. See ThrowOutcome's doc comment in vm.h.

    if (m_errorClass == nullptr) {
        // Error class not ready; fallback to uncaught error
        runtimeError("%s", msg);
        return ThrowOutcome::Uncaught;
    }

    // GC safety: root msg_obj/kind_obj only for the window before err_obj
    // holds them. Once err_obj exists, its own traceObject marks both, so
    // their liveness after that rides on err_obj staying reachable —
    // guaranteed for the whole call below by handleThrow()'s own root on
    // thrownValue (see its comment), not by anything here.
    ObjString* msg_obj = m_mm.makeString(msg);
    m_mm.pushTempRoot(msg_obj);

    ObjString* kind_obj = m_mm.makeString(kind_str);
    m_mm.pushTempRoot(kind_obj);

    ObjError* err_obj = m_mm.create<ObjError>(m_errorClass, msg_obj, kind_obj);

    m_mm.popTempRoot(); // Unroot kind_obj
    m_mm.popTempRoot(); // Unroot msg_obj

    return handleThrow(Value{static_cast<Obj*>(err_obj)}, stopAtFrameCount);
}

InterpretResult VM::run(int stopAtFrameCount) {
#define RAISE_ERROR(...)                                                       \
    do {                                                                       \
        frame->ip = ip;                                                        \
        runtimeError(__VA_ARGS__);                                             \
    } while (false)

    // tryCatchableError(kind, msg) returns ThrowOutcome (see vm.h). This
    // macro reacts to it exactly the way every catchable-fault call site
    // must: return RUNTIME_ERROR if uncaught, return OK immediately (without
    // touching frame/ip/chunk — they may not even be safe to read, see
    // ThrowOutcome's doc comment) if the catch resolved outside this run()
    // invocation's own frame range, and otherwise do nothing, so the call
    // site's own trailing `break;` resumes dispatch normally. Only `return`
    // appears in this macro's body — never `break`/`continue` — so it is
    // safe to expand inside another do/while (BINARY_OP) or directly inside
    // a switch case without an enclosing loop/switch swallowing a break that
    // was meant for the opcode dispatch switch.
#define CATCHABLE_OR_RETURN(outcome_expr)                                      \
    do {                                                                       \
        ThrowOutcome _catchableOutcome = (outcome_expr);                       \
        if (_catchableOutcome == ThrowOutcome::Uncaught) {                     \
            return InterpretResult::RUNTIME_ERROR;                             \
        }                                                                      \
        if (_catchableOutcome == ThrowOutcome::HandledStop) {                  \
            return InterpretResult::OK;                                        \
        }                                                                      \
    } while (false)

#define BINARY_OP(valueType, op, kind_str, msg)                                \
    do {                                                                       \
        if (!is<Number>(peek(0)) || !is<Number>(peek(1))) {                    \
            CATCHABLE_OR_RETURN(tryCatchableError(kind_str, msg));             \
            break;                                                             \
        }                                                                      \
        Number b = as<Number>(pop());                                          \
        Number a = as<Number>(pop());                                          \
        push(as<valueType>(a op b));                                           \
    } while (false)

    // Local to VM::run(). The single place that knows how to derive the
    // register-cached ip/chunk from the live CallFrame array — used for the
    // initial load below, for Op::RETURN's reload, and (via the constructor/
    // destructor pair) to bracket one call()/callNative()/callBoundNative()/
    // bindMethod() invocation: flush the register-cached ip into frame->ip on
    // construction (so a runtimeError() raised inside sees an up-to-date
    // stack trace), then reload frame/ip/chunk from the current top of
    // m_frames on destruction (a new frame if one was pushed, the same frame
    // — a no-op reload — otherwise). The guard form is always applied at a
    // call/bindMethod site, whether or not the call actually changes
    // frameCount, so no call site has to reason about which case it is.
    class FrameSync {
      public:
        // Unconditional: derive frame/ip/chunk from the top of m_frames.
        // Requires frameCount > 0 — true for every caller of this overload
        // (the initial load below, and Op::RETURN, which already checks
        // m_frameCount == 0 and returns before reaching it).
        static void loadTop(CallFrame* frames, int frameCount,
                            CallFrame*& frame, Chunk::const_iterator& ip,
                            const Chunk*& chunk) {
            frame = &frames[frameCount - 1];
            ip = frame->ip;
            chunk = &frame->closure->function->chunk;
        }

        FrameSync(CallFrame* frames, int& frameCount, CallFrame*& frame,
                  Chunk::const_iterator& ip, const Chunk*& chunk)
            : m_frames(frames), m_frameCount(frameCount), m_frame(frame),
              m_ip(ip), m_chunk(chunk) {
            m_frame->ip = m_ip; // flush
        }
        ~FrameSync() {
            // A runtimeError() raised by the guarded call (arity mismatch,
            // stack overflow, undefined property, ...) resets the frame
            // stack to empty before returning failure — m_frameCount == 0
            // here means there is no frame left to resync from. Leave
            // frame/ip/chunk stale; the caller returns
            // InterpretResult::RUNTIME_ERROR immediately without reading
            // them again.
            if (m_frameCount > 0) {
                loadTop(m_frames, m_frameCount, m_frame, m_ip, m_chunk);
            }
        }
        FrameSync(const FrameSync&) = delete;
        FrameSync& operator=(const FrameSync&) = delete;

      private:
        CallFrame* m_frames;
        int& m_frameCount;
        CallFrame*& m_frame;
        Chunk::const_iterator& m_ip;
        const Chunk*& m_chunk;
    };

    CallFrame* frame;
    Chunk::const_iterator ip;
    const Chunk* chunk; // constant-pool base
    FrameSync::loadTop(m_frames, m_frameCount, frame, ip, chunk);

    auto readByte = [&ip]() -> Byte { return *ip++; };
    auto readShort = [&readByte]() -> uint16_t {
        uint16_t hi = readByte();
        uint16_t lo = readByte();
        return static_cast<uint16_t>((hi << 8) | lo);
    };

    // Helper lambda to construct and potentially catch a runtime error.
    // Syncs frame->ip before any operation, then forwards to the shared
    // raiseThrowableError with THIS run() invocation's own stopAtFrameCount
    // — the boundary a reentrant fault must be judged against, not always 0
    // (see ThrowOutcome's doc comment in vm.h; using the wrong boundary here
    // is exactly what let a caught fault corrupt an unrelated, more-nested
    // run() invocation's frame bookkeeping). On HandledContinue, reloads
    // frame/ip/chunk from the new top — safe, since m_frameCount is still
    // above stopAtFrameCount. On HandledStop or Uncaught, frame/ip/chunk are
    // deliberately left untouched: CATCHABLE_OR_RETURN returns from run()
    // before either is read again, and reading them here could be out of
    // bounds (m_frameCount may be 0).
    auto tryCatchableError = [this, &frame, &ip, &chunk, stopAtFrameCount](
                                 const char* kind_str,
                                 const char* msg) -> ThrowOutcome {
        frame->ip = ip; // Sync frame->ip before allocations (fixes line number)

        ThrowOutcome outcome =
            raiseThrowableError(kind_str, msg, stopAtFrameCount);
        if (outcome == ThrowOutcome::HandledContinue) {
            FrameSync::loadTop(m_frames, m_frameCount, frame, ip, chunk);
        }
        return outcome;
    };

    auto readConstant = [&chunk, &readShort]() -> Value {
        return chunk->getConstant(readShort());
    };

    for (;;) {
        if (m_stackOverflow) {
            // Consume the flag now: whichever branch below runs, this exact
            // overflow event is fully handled by it (caught, reported, or
            // re-armed by a fresh push() past the threshold later).
            m_stackOverflow = false;
            if (!m_handlerStack.empty() && !m_unwindingStackOverflow) {
                // See m_unwindingStackOverflow's own comment (vm.h) and
                // VM::call()'s matching guard: hold the flag for exactly
                // this catch attempt, so a nested hit (from a deferred call
                // this unwind drains) falls through to the fatal branch
                // below instead of recursing.
                m_unwindingStackOverflow = true;
                ThrowOutcome outcome =
                    tryCatchableError("StackOverflowError", "Stack overflow.");
                m_unwindingStackOverflow = false;
                CATCHABLE_OR_RETURN(outcome);
            } else {
                RAISE_ERROR("Stack overflow.");
                return InterpretResult::RUNTIME_ERROR;
            }
        }

#ifdef LOXPP_DEBUG_TRACE_EXECUTION
        {
            int currentOffset = static_cast<int>(ip - chunk->cbegin());
            bool color = isatty(STDOUT_FILENO) != 0;
            std::printf("[line %d] ", chunk->getLine(currentOffset));
            std::printf("          ");
            for (Value* slot = stack; slot < stackTop; slot++) {
                std::printf("[ ");
                printValue(*slot);
                std::printf(" ]");
            }
            std::printf("\n");
            disassembleInstruction(*chunk, m_mm, currentOffset, std::cout,
                                   color);
        }
#endif

        Byte instruction = readByte();
#ifdef LOXPP_PROFILE
        m_profilerData.opcodeTable[instruction].count++;
#endif
        switch (toOpcode(instruction)) {
        case Op::CONSTANT: {
            push(readConstant());
            break;
        }
        case Op::NIL: {
            push(from<Nil>(Nil{}));
            break;
        }
        case Op::TRUE: {
            push(from<bool>(true));
            break;
        }
        case Op::FALSE: {
            push(from<bool>(false));
            break;
        }
        case Op::EQUAL: {
            Value b = pop();
            Value a = pop();
            push(from<bool>(a == b));
            break;
        }
        case Op::GREATER: {
            BINARY_OP(bool, >, "ComparisonTypeError",
                      "Operands must be numbers.");
            break;
        }
        case Op::LESS: {
            BINARY_OP(bool, <, "ComparisonTypeError",
                      "Operands must be numbers.");
            break;
        }
        case Op::NEGATE: {
            if (!is<Number>(peek(0))) {
                CATCHABLE_OR_RETURN(tryCatchableError(
                    "ArithmeticTypeError", "Operand must be a number."));
                break;
            }
            push(from<Number>(-as<Number>(pop())));
            break;
        }
        case Op::ADD: {
            if (isString(peek(0)) && isString(peek(1))) {
                auto* b_str = asObjString(pop());
                auto* a_str = asObjString(pop());
                std::string result;
                result.reserve(a_str->chars.size() + b_str->chars.size());
                result.append(a_str->chars.data(), a_str->chars.size());
                result.append(b_str->chars.data(), b_str->chars.size());
                push(Value{
                    static_cast<Obj*>(m_mm.makeString(std::move(result)))});
            } else {
                BINARY_OP(Number, +, "ConcatenationTypeError",
                          "Operands must be two numbers, two strings, or a "
                          "string and a number.");
            }
            break;
        }
        case Op::SUBTRACT: {
            BINARY_OP(Number, -, "ArithmeticTypeError",
                      "Operands must be numbers.");
            break;
        }
        case Op::MULTIPLY: {
            BINARY_OP(Number, *, "ArithmeticTypeError",
                      "Operands must be numbers.");
            break;
        }
        case Op::DIVIDE: {
            BINARY_OP(Number, /, "ArithmeticTypeError",
                      "Operands must be numbers.");
            break;
        }
        case Op::MODULO: {
            if (!is<Number>(peek(0)) || !is<Number>(peek(1))) {
                CATCHABLE_OR_RETURN(tryCatchableError(
                    "ArithmeticTypeError", "Operands must be numbers."));
                break;
            }
            Number b = as<Number>(pop());
            Number a = as<Number>(pop());
            Number result = std::fmod(a, b);
            // Floor-division semantics: result has same sign as b (Python/Lua
            // behavior)
            if (result != 0 && (result < 0) != (b < 0)) {
                result += b;
            }
            push(from<Number>(result));
            break;
        }
        case Op::NOT: {
            push(from<bool>(!pop()));
            break;
        }
        case Op::PRINT: {
            m_stdlibCtx.clearError();
            std::string s = stringify(pop());
            if (m_stdlibCtx.nativeError) {
                RAISE_ERROR("%s", m_stdlibCtx.nativeErrorMsg.c_str());
                return InterpretResult::RUNTIME_ERROR;
            }
            std::fwrite(s.data(), 1, s.size(), stdout);
            std::printf("\n");
            break;
        }
        case Op::POP: {
            m_lastResult = pop();
            break;
        }
        case Op::GET_LOCAL: {
            uint8_t slot = readByte();
            push(frame->slots[slot]);
            break;
        }
        case Op::SET_LOCAL: {
            uint8_t slot = readByte();
            // assignment is an expression; leave value on stack
            frame->slots[slot] = peek(0);
            break;
        }
        case Op::DEFINE_GLOBAL: {
            ObjString* name = asObjString(readConstant());
            m_globals.set(name, peek(0));
            pop();
            break;
        }
        case Op::GET_GLOBAL: {
            ObjString* name = asObjString(readConstant());
            Value value;
            if (!m_globals.get(name, value)) {
                CATCHABLE_OR_RETURN(tryCatchableError("UndefinedVariableError",
                                                      "Undefined variable."));
                break;
            }
            push(value);
            break;
        }
        case Op::SET_GLOBAL: {
            ObjString* name = asObjString(readConstant());
            // set() returns true if the key is *new*; an existing key is
            // valid. An entirely new key means the variable was never
            // declared.
            if (m_globals.set(name, peek(0))) {
                m_globals.del(name); // undo the spurious insertion
                CATCHABLE_OR_RETURN(tryCatchableError("UndefinedVariableError",
                                                      "Undefined variable."));
                break;
            }
            break;
        }
        case Op::JUMP: {
            ip += readShort();
            break;
        }
        case Op::JUMP_IF_FALSE: {
            uint16_t offset = readShort();
            if (isFalsy(peek(0))) {
                ip += offset;
            }
            break;
        }
        case Op::LOOP: {
            ip -= readShort();
            break;
        }
        case Op::MATCH_ERROR: {
            CATCHABLE_OR_RETURN(tryCatchableError(
                "MatchError", "No matching arm in match expression."));
            break;
        }
        case Op::JUMP_TABLE: {
            uint8_t minTag = readByte();
            uint8_t count = readByte();
            auto tableBase = ip; // iterator to entry[0]
            ip += static_cast<int>(count) * 2;
            Value tagVal = pop();
            int tag = static_cast<int>(as<Number>(tagVal));
            int idx = tag - static_cast<int>(minTag);
            if (idx >= 0 && idx < static_cast<int>(count)) {
                uint16_t fwd = static_cast<uint16_t>((tableBase[idx * 2] << 8) |
                                                     tableBase[(idx * 2) + 1]);
                ip += fwd;
            }
            break;
        }
        case Op::GET_TAG: {
            Value val = pop();
            if (!isEnumValue(val)) {
                RAISE_ERROR("GET_TAG: expected an enum value.");
                return InterpretResult::RUNTIME_ERROR;
            }
            auto tag = static_cast<double>(asObjEnum(as<Obj*>(val))->ctor->tag);
            push(Value{tag});
            break;
        }
        case Op::IS_SEQ: {
            Value val = pop();
            push(Value{isList(val) || isString(val)});
            break;
        }
        case Op::INSTANCEOF: {
            ObjString* className = asObjString(readConstant());
            Value val = pop();
            Value classVal;
            bool result = false;
            if (m_globals.get(className, classVal) && isClass(classVal)) {
                ObjClass* target = asObjClass(as<Obj*>(classVal));
                if (isInstance(val)) {
                    ObjClass* klass = asObjInstance(as<Obj*>(val))->klass;
                    const ObjClass* found = walkChain<ObjClass>(
                        klass,
                        [target](const ObjClass* k) { return k == target; },
                        [](const ObjClass* k) { return k->superclass; });
                    result = found != nullptr;
                }
            }
            push(Value{result});
            break;
        }
        case Op::CALL: {
            int argCount = readByte();
            Value callee = peek(argCount);
            if (isNative(callee)) {
                FrameSync sync(m_frames, m_frameCount, frame, ip, chunk);
                if (!callNative(asObjNative(callee), argCount)) {
                    return InterpretResult::RUNTIME_ERROR;
                }
            } else if (isClosure(callee)) {
                FrameSync sync(m_frames, m_frameCount, frame, ip, chunk);
                CallOutcome outcome =
                    call(asObjClosure(callee), argCount, stopAtFrameCount);
                if (outcome == CallOutcome::Uncaught) {
                    return InterpretResult::RUNTIME_ERROR;
                }
                if (outcome == CallOutcome::CaughtStop) {
                    return InterpretResult::OK;
                }
            } else if (isBoundMethod(callee)) {
                ObjBoundMethod* bound = asObjBoundMethod(as<Obj*>(callee));
                // Slot 0 of the new frame = receiver (= this).
                stackTop[-argCount - 1] = bound->receiver;
                FrameSync sync(m_frames, m_frameCount, frame, ip, chunk);
                CallOutcome outcome =
                    call(bound->method, argCount, stopAtFrameCount);
                if (outcome == CallOutcome::Uncaught) {
                    return InterpretResult::RUNTIME_ERROR;
                }
                if (outcome == CallOutcome::CaughtStop) {
                    return InterpretResult::OK;
                }
            } else if (isBoundNative(callee)) {
                ObjBoundNative* bn = asObjBoundNative(as<Obj*>(callee));
                FrameSync sync(m_frames, m_frameCount, frame, ip, chunk);
                if (!callBoundNative(bn, argCount)) {
                    return InterpretResult::RUNTIME_ERROR;
                }
            } else if (isClass(callee)) {
                ObjClass* klass = asObjClass(as<Obj*>(callee));
                ObjInstance* instance =
                    m_mm.create<ObjInstance>(klass, VmAllocator<Entry>{&m_mm});
                stackTop[-argCount - 1] = Value{static_cast<Obj*>(instance)};
                // Call init() if the class defines one.
                ObjString* initStr = m_mm.findString("init");
                Value initMethod;
                if (initStr && klass->methods.get(initStr, initMethod)) {
                    FrameSync sync(m_frames, m_frameCount, frame, ip, chunk);
                    CallOutcome outcome =
                        call(asObjClosure(as<Obj*>(initMethod)), argCount,
                             stopAtFrameCount);
                    if (outcome == CallOutcome::Uncaught) {
                        return InterpretResult::RUNTIME_ERROR;
                    }
                    if (outcome == CallOutcome::CaughtStop) {
                        return InterpretResult::OK;
                    }
                } else if (argCount != 0) {
                    CATCHABLE_OR_RETURN(tryCatchableError(
                        "ConstructorArityError",
                        "Expected 0 arguments but got some."));
                    break;
                }
            } else if (isEnumCtor(callee)) {
                ObjEnumCtor* ctor = asObjEnumCtor(as<Obj*>(callee));
                if (argCount != static_cast<int>(ctor->arity)) {
                    CATCHABLE_OR_RETURN(tryCatchableError(
                        "ConstructorArityError",
                        "Constructor called with wrong arity."));
                    break;
                }
                ObjEnum* enumVal =
                    m_mm.create<ObjEnum>(ctor, VmAllocator<Value>{&m_mm});
                m_mm.pushTempRoot(enumVal);
                enumVal->fields.resize(static_cast<size_t>(argCount));
                for (int i = argCount - 1; i >= 0; i--) {
                    enumVal->fields[static_cast<size_t>(i)] = pop();
                }
                m_mm.popTempRoot();
                pop(); // pop the ObjEnumCtor from the callee slot
                push(Value{static_cast<Obj*>(enumVal)});
            } else {
                CATCHABLE_OR_RETURN(tryCatchableError(
                    "NotCallableError",
                    "Can only call functions, classes and enums."));
                break;
            }
            break;
        }
        case Op::CLASS: {
            ObjString* name = asObjString(readConstant());
            ObjClass* klass =
                m_mm.create<ObjClass>(name, VmAllocator<Entry>{&m_mm});
            push(Value{static_cast<Obj*>(klass)});
            break;
        }
        case Op::GET_PROPERTY: {
            if (isError(peek(0))) {
                ObjError* err = asObjError(as<Obj*>(peek(0)));
                ObjString* name = asObjString(readConstant());
                pop(); // error
                if (name->chars == "message") {
                    push(Value{static_cast<Obj*>(err->message)});
                } else if (name->chars == "kind") {
                    push(Value{static_cast<Obj*>(err->kind)});
                } else {
                    CATCHABLE_OR_RETURN(
                        tryCatchableError("UndefinedPropertyError",
                                          "Undefined property on error."));
                    break;
                }
                break;
            }
            if (isFile(peek(0))) {
                ObjString* name = asObjString(readConstant());
                Value method;
                if (!m_fileClass->methods.get(name, method)) {
                    RAISE_ERROR("Undefined property '%s' on file.",
                                name->chars.c_str());
                    return InterpretResult::RUNTIME_ERROR;
                }
                ObjBoundNative* bound = m_mm.create<ObjBoundNative>(
                    peek(0), asObjNative(as<Obj*>(method)));
                pop(); // file
                push(Value{static_cast<Obj*>(bound)});
                break;
            }
            if (isMap(peek(0))) {
                ObjString* name = asObjString(readConstant());
                Value method;
                if (!m_mapClass->methods.get(name, method)) {
                    RAISE_ERROR("Undefined property '%s' on map.",
                                name->chars.c_str());
                    return InterpretResult::RUNTIME_ERROR;
                }
                ObjBoundNative* bound = m_mm.create<ObjBoundNative>(
                    peek(0), asObjNative(as<Obj*>(method)));
                pop(); // map
                push(Value{static_cast<Obj*>(bound)});
                break;
            }
            if (!isInstance(peek(0))) {
                RAISE_ERROR("Only instances have properties.");
                return InterpretResult::RUNTIME_ERROR;
            }
            ObjInstance* instance = asObjInstance(as<Obj*>(peek(0)));
            ObjString* name = asObjString(readConstant());
            Value value;
            if (instance->fields.get(name, value)) {
                pop(); // instance
                push(value);
                break;
            }
            {
                FrameSync sync(m_frames, m_frameCount, frame, ip, chunk);
                if (!bindMethod(instance->klass, name)) {
                    return InterpretResult::RUNTIME_ERROR;
                }
            }
            break;
        }
        case Op::SET_PROPERTY: {
            if (!isInstance(peek(1))) {
                RAISE_ERROR("Only instances have fields.");
                return InterpretResult::RUNTIME_ERROR;
            }
            ObjInstance* instance = asObjInstance(as<Obj*>(peek(1)));
            ObjString* name = asObjString(readConstant());
            instance->fields.set(name, peek(0));
            Value val = pop(); // value
            pop();             // instance
            push(val);         // assignment is an expression
            break;
        }
        case Op::DEFINE_METHOD: {
            ObjString* name = asObjString(readConstant());
            Value method = peek(0); // ObjClosure* on top
            ObjClass* klass = asObjClass(as<Obj*>(peek(1))); // class below
            klass->methods.set(name, method);
            pop(); // pop closure; leave class on stack for next method
            break;
        }
        case Op::INVOKE: {
            ObjString* name = asObjString(readConstant());
            int argCount = readByte();
            Value receiver = peek(argCount);
            if (isInstance(receiver)) {
                ObjInstance* instance = asObjInstance(as<Obj*>(receiver));
                // A field can shadow a method — check fields first.
                Value fieldVal;
                if (instance->fields.get(name, fieldVal)) {
                    stackTop[-argCount - 1] = fieldVal;
                    FrameSync sync(m_frames, m_frameCount, frame, ip, chunk);
                    if (isClosure(fieldVal)) {
                        CallOutcome outcome =
                            call(asObjClosure(as<Obj*>(fieldVal)), argCount,
                                 stopAtFrameCount);
                        if (outcome == CallOutcome::Uncaught) {
                            return InterpretResult::RUNTIME_ERROR;
                        }
                        if (outcome == CallOutcome::CaughtStop) {
                            return InterpretResult::OK;
                        }
                    } else if (isNative(fieldVal)) {
                        if (!callNative(asObjNative(as<Obj*>(fieldVal)),
                                        argCount)) {
                            return InterpretResult::RUNTIME_ERROR;
                        }
                    } else if (isBoundNative(fieldVal)) {
                        ObjBoundNative* bn =
                            asObjBoundNative(as<Obj*>(fieldVal));
                        if (!callBoundNative(bn, argCount)) {
                            return InterpretResult::RUNTIME_ERROR;
                        }
                    } else {
                        RAISE_ERROR(
                            "Can only call functions, classes and enums.");
                        return InterpretResult::RUNTIME_ERROR;
                    }
                    break;
                }
                // Fast path: call the method directly — receiver already sits
                // at stackTop[-argCount-1], which becomes slot 0 (= this) of
                // the new frame.
                Value method;
                if (!instance->klass->methods.get(name, method)) {
                    RAISE_ERROR("Undefined property '%s'.",
                                name->chars.c_str());
                    return InterpretResult::RUNTIME_ERROR;
                }
                Obj* methodObj = as<Obj*>(method);
                FrameSync sync(m_frames, m_frameCount, frame, ip, chunk);
                if (isObjNative(methodObj)) {
                    if (!callNative(asObjNative(methodObj), argCount)) {
                        return InterpretResult::RUNTIME_ERROR;
                    }
                } else {
                    CallOutcome outcome = call(asObjClosure(methodObj),
                                               argCount, stopAtFrameCount);
                    if (outcome == CallOutcome::Uncaught) {
                        return InterpretResult::RUNTIME_ERROR;
                    }
                    if (outcome == CallOutcome::CaughtStop) {
                        return InterpretResult::OK;
                    }
                }
            } else if (isList(receiver)) {
                ObjList* list = asObjList(as<Obj*>(receiver));
                if (name->chars == "append") {
                    if (argCount != 1) {
                        RAISE_ERROR("'append' expects 1 argument but got %d.",
                                    argCount);
                        return InterpretResult::RUNTIME_ERROR;
                    }
                    Value val =
                        peek(0); // still on stack — GC-safe during push_back
                    list->elements.push_back(val);
                    pop(); // arg
                    pop(); // receiver
                    push(from<Nil>(Nil{}));
                } else if (name->chars == "pop") {
                    if (argCount != 0) {
                        RAISE_ERROR("'pop' expects 0 arguments but got %d.",
                                    argCount);
                        return InterpretResult::RUNTIME_ERROR;
                    }
                    if (list->elements.empty()) {
                        CATCHABLE_OR_RETURN(tryCatchableError(
                            "EmptyListError",
                            "Cannot pop from an empty list."));
                        break;
                    }
                    Value val = list->elements.back();
                    list->elements.pop_back();
                    pop(); // receiver
                    push(val);
                } else if (name->chars == "remove") {
                    if (argCount != 1) {
                        RAISE_ERROR("'remove' expects 1 argument but got %d.",
                                    argCount);
                        return InterpretResult::RUNTIME_ERROR;
                    }
                    Value target = peek(0);
                    auto& elems = list->elements;
                    bool found = false;
                    for (int i = 0; i < static_cast<int>(elems.size()); i++) {
                        if (elems[i] == target) {
                            elems.erase(elems.begin() + i);
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        RAISE_ERROR("Value not found in list.");
                        return InterpretResult::RUNTIME_ERROR;
                    }
                    pop(); // arg
                    pop(); // receiver
                    push(from<Nil>(Nil{}));
                } else {
                    RAISE_ERROR("Undefined method '%s' on list.",
                                name->chars.c_str());
                    return InterpretResult::RUNTIME_ERROR;
                }
            } else if (isFile(receiver)) {
                Value method;
                if (!m_fileClass->methods.get(name, method)) {
                    RAISE_ERROR("Undefined method '%s' on file.",
                                name->chars.c_str());
                    return InterpretResult::RUNTIME_ERROR;
                }
                FrameSync sync(m_frames, m_frameCount, frame, ip, chunk);
                if (!callNative(asObjNative(as<Obj*>(method)), argCount)) {
                    return InterpretResult::RUNTIME_ERROR;
                }
            } else if (isMap(receiver)) {
                Value method;
                if (!m_mapClass->methods.get(name, method)) {
                    RAISE_ERROR("Undefined method '%s' on map.",
                                name->chars.c_str());
                    return InterpretResult::RUNTIME_ERROR;
                }
                FrameSync sync(m_frames, m_frameCount, frame, ip, chunk);
                if (!callNative(asObjNative(as<Obj*>(method)), argCount)) {
                    return InterpretResult::RUNTIME_ERROR;
                }
            } else {
                CATCHABLE_OR_RETURN(
                    tryCatchableError("InvalidReceiverError",
                                      "Method called on invalid receiver."));
                break;
            }
            break;
        }
        case Op::INHERIT: {
            Value superVal = peek(1);
            if (!isClass(superVal)) {
                RAISE_ERROR("Superclass must be a class.");
                return InterpretResult::RUNTIME_ERROR;
            }
            ObjClass* superclass = asObjClass(as<Obj*>(superVal));
            ObjClass* subclass = asObjClass(as<Obj*>(peek(0)));
            subclass->methods.addAll(superclass->methods);
            subclass->superclass = superclass;
            pop(); // pop subclass; superclass stays as "super" local
            break;
        }
        case Op::GET_SUPER: {
            ObjString* name = asObjString(readConstant());
            ObjClass* superclass = asObjClass(as<Obj*>(pop()));
            FrameSync sync(m_frames, m_frameCount, frame, ip, chunk);
            if (!bindMethod(superclass, name)) {
                return InterpretResult::RUNTIME_ERROR;
            }
            break;
        }
        case Op::SUPER_INVOKE: {
            ObjString* name = asObjString(readConstant());
            int argCount = readByte();
            ObjClass* superclass = asObjClass(as<Obj*>(pop()));
            Value method;
            if (!superclass->methods.get(name, method)) {
                RAISE_ERROR("Undefined property '%s'.", name->chars.c_str());
                return InterpretResult::RUNTIME_ERROR;
            }
            FrameSync sync(m_frames, m_frameCount, frame, ip, chunk);
            {
                CallOutcome outcome = call(asObjClosure(as<Obj*>(method)),
                                           argCount, stopAtFrameCount);
                if (outcome == CallOutcome::Uncaught) {
                    return InterpretResult::RUNTIME_ERROR;
                }
                if (outcome == CallOutcome::CaughtStop) {
                    return InterpretResult::OK;
                }
            }
            break;
        }
        case Op::CLOSURE: {
            ObjFunction* fn = asObjFunction(readConstant());
            ObjClosure* cl = m_mm.create<ObjClosure>(fn);
            push(Value{static_cast<Obj*>(cl)});
            for (int i = 0; i < fn->upvalueCount; i++) {
                uint8_t isLocal = readByte();
                uint8_t index = readByte();
                if (isLocal) {
                    cl->upvalues[i] = captureUpvalue(frame->slots + index);
                } else {
                    cl->upvalues[i] = frame->closure->upvalues[index];
                }
            }
            break;
        }
        case Op::GET_UPVALUE: {
            uint8_t slot = readByte();
            push(*frame->closure->upvalues[slot]->location);
            break;
        }
        case Op::SET_UPVALUE: {
            uint8_t slot = readByte();
            *frame->closure->upvalues[slot]->location = peek(0);
            break;
        }
        case Op::CLOSE_UPVALUE: {
            closeUpvalues(stackTop - 1);
            pop();
            break;
        }
        case Op::RETURN: {
            Value result = pop();
            closeUpvalues(frame->slots);
            // A defer-free function has no RUN_DEFERS, so this is the only
            // place its own stale records get discarded. A function with
            // defers already had this done by RUN_DEFERS below, before its
            // defers ran; this is then a no-op.
            popHandlersOwnedByCurrentFrame();
#ifdef LOXPP_PROFILE
            // Destroy the function scope before decrementing frameCount so the
            // depth index still points to this frame's slot.
            m_profilerScopes[m_frameCount - 1].reset();
#endif
            m_frameCount--;
            if (m_frameCount == 0) {
                // Finished executing the top-level script.
                pop(); // remove the script ObjClosure from the stack
                return InterpretResult::OK;
            }
            // Discard the callee's stack window and push return value.
            stackTop = frame->slots;
            push(result);
            FrameSync::loadTop(m_frames, m_frameCount, frame, ip, chunk);
            if (m_frameCount <= stopAtFrameCount) {
                // A nested run() (draining a deferred call — see
                // runPendingDefers) reached the depth it was asked to stop
                // at; hand control back to whichever C++ frame started it.
                return InterpretResult::OK;
            }
            break;
        }
        case Op::BUILD_LIST: {
            uint8_t count = readByte();
            ObjList* list = m_mm.create<ObjList>(VmAllocator<Value>{&m_mm});
            m_mm.pushTempRoot(list); // protect across resize's potential GC
            list->elements.resize(count);
            for (int i = count - 1; i >= 0; i--) {
                list->elements[i] = pop();
            }
            m_mm.popTempRoot();
            push(Value{static_cast<Obj*>(list)});
            break;
        }
        case Op::BUILD_MAP: {
            uint8_t count = readByte();
            // Validate all keys before any allocation. Stack (top to bottom):
            //   val_{n-1}, key_{n-1}, ..., val_0, key_0
            bool errorCaught = false;
            for (int i = 0; i < count; i++) {
                Value key = peek(2 * (count - 1 - i) + 1);
                // Check for NaN first
                if (is<Number>(key) && std::isnan(as<Number>(key))) {
                    // The macro's own early returns cover Uncaught/HandledStop;
                    // reaching here means HandledContinue, so break out of this
                    // validation loop (not the outer switch — errorCaught does
                    // that below) same as before.
                    CATCHABLE_OR_RETURN(tryCatchableError(
                        "NaNKeyError", "NaN cannot be used as a map key."));
                    errorCaught = true;
                    break;
                }
                // Then check for invalid object types (non-String)
                if (is<Obj*>(key) && as<Obj*>(key)->type != ObjType::STRING) {
                    CATCHABLE_OR_RETURN(tryCatchableError(
                        "InvalidMapKeyError",
                        "Map keys must be Bool, Number, Nil, or String."));
                    errorCaught = true;
                    break;
                }
            }
            if (errorCaught) {
                break;
            }
            ObjMap* map =
                m_mm.create<ObjMap>(m_mapClass, VmAllocator<MapEntry>{&m_mm});
            // Values are still on the stack → GC-rooted; map is temp-rooted
            // so it survives any GC triggered by mapSet's grow.
            m_mm.pushTempRoot(map);
            for (int i = 0; i < count; i++) {
                Value key = peek(2 * (count - 1 - i) + 1);
                Value val = peek(2 * (count - 1 - i));
                map->mapSet(key, val);
            }
            m_mm.popTempRoot();
            for (int i = 0; i < 2 * count; i++) {
                pop();
            }
            push(Value{static_cast<Obj*>(map)});
            break;
        }
        case Op::GET_INDEX: {
            Value indexVal = pop();
            Value collectionVal = pop();
            if (isList(collectionVal)) {
                if (!is<Number>(indexVal)) {
                    CATCHABLE_OR_RETURN(tryCatchableError(
                        "IndexTypeError", "List index must be a number."));
                    break;
                }
                double n = as<Number>(indexVal);
                if (n != std::floor(n)) {
                    CATCHABLE_OR_RETURN(
                        tryCatchableError("IndexNotIntegerError",
                                          "List index must be an integer."));
                    break;
                }
                auto* list = asObjList(as<Obj*>(collectionVal));
                int idx = static_cast<int>(n);
                if (idx < 0 || idx >= static_cast<int>(list->elements.size())) {
                    CATCHABLE_OR_RETURN(tryCatchableError(
                        "IndexOutOfBoundsError", "List index out of bounds."));
                    break;
                }
                push(list->elements[idx]);
            } else if (isString(collectionVal)) {
                if (!is<Number>(indexVal)) {
                    CATCHABLE_OR_RETURN(tryCatchableError(
                        "IndexTypeError", "String index must be a number."));
                    break;
                }
                double n = as<Number>(indexVal);
                if (n != std::floor(n)) {
                    CATCHABLE_OR_RETURN(
                        tryCatchableError("IndexNotIntegerError",
                                          "String index must be an integer."));
                    break;
                }
                auto* str = asObjString(as<Obj*>(collectionVal));
                int idx = static_cast<int>(n);
                if (idx < 0 || idx >= static_cast<int>(str->chars.size())) {
                    CATCHABLE_OR_RETURN(
                        tryCatchableError("IndexOutOfBoundsError",
                                          "String index out of bounds."));
                    break;
                }
                // Copy char before makeString (GC-safe: same pattern as ADD)
                char ch = str->chars[idx];
                push(Value{static_cast<Obj*>(
                    m_mm.makeString(std::string_view{&ch, 1}))});
            } else if (isMap(collectionVal)) {
                if (is<Number>(indexVal) && std::isnan(as<Number>(indexVal))) {
                    CATCHABLE_OR_RETURN(tryCatchableError(
                        "NaNKeyError", "NaN cannot be used as a map key."));
                    break;
                }
                if (is<Obj*>(indexVal) &&
                    as<Obj*>(indexVal)->type != ObjType::STRING) {
                    CATCHABLE_OR_RETURN(tryCatchableError(
                        "InvalidMapKeyError",
                        "Map keys must be Bool, Number, Nil, or String."));
                    break;
                }
                auto* map = asObjMap(as<Obj*>(collectionVal));
                Value result{Nil{}}; // default nil — returned when key absent
                map->mapGet(indexVal, result);
                push(result);
            } else if (isEnumValue(collectionVal)) {
                if (!is<Number>(indexVal)) {
                    RAISE_ERROR("Enum field index must be a number.");
                    return InterpretResult::RUNTIME_ERROR;
                }
                double n = as<Number>(indexVal);
                auto* e = asObjEnum(as<Obj*>(collectionVal));
                int idx = static_cast<int>(n);
                if (idx < 0 || idx >= static_cast<int>(e->fields.size())) {
                    RAISE_ERROR("Enum field index %d out of range.", idx);
                    return InterpretResult::RUNTIME_ERROR;
                }
                push(e->fields[static_cast<size_t>(idx)]);
            } else {
                CATCHABLE_OR_RETURN(tryCatchableError(
                    "NotIndexableError",
                    "Only lists, strings, and maps can be indexed."));
                break;
            }
            break;
        }
        case Op::SET_INDEX: {
            Value val = pop();
            Value indexVal = pop();
            Value listVal = pop();
            if (isString(listVal)) {
                RAISE_ERROR("Strings are immutable and cannot be indexed for "
                            "assignment.");
                return InterpretResult::RUNTIME_ERROR;
            }
            if (isMap(listVal)) {
                if (is<Number>(indexVal) && std::isnan(as<Number>(indexVal))) {
                    CATCHABLE_OR_RETURN(tryCatchableError(
                        "NaNKeyError", "NaN cannot be used as a map key."));
                    break;
                }
                if (is<Obj*>(indexVal) &&
                    as<Obj*>(indexVal)->type != ObjType::STRING) {
                    CATCHABLE_OR_RETURN(tryCatchableError(
                        "InvalidMapKeyError",
                        "Map keys must be Bool, Number, Nil, or String."));
                    break;
                }
                auto* map = asObjMap(as<Obj*>(listVal));
                // Root the map: it was popped and may be a temporary; mapSet
                // can grow the bucket array which triggers GC.
                m_mm.pushTempRoot(map);
                // Root val if it's an object: it was popped off the stack
                // before mapSet, so the GC won't find it through the stack.
                if (is<Obj*>(val)) {
                    m_mm.pushTempRoot(as<Obj*>(val));
                }
                map->mapSet(indexVal, val);
                if (is<Obj*>(val)) {
                    m_mm.popTempRoot();
                }
                m_mm.popTempRoot();
                push(val);
                break;
            }
            if (!isList(listVal)) {
                CATCHABLE_OR_RETURN(tryCatchableError(
                    "NotIndexableError",
                    "Only lists and maps can be indexed for assignment."));
                break;
            }
            if (!is<Number>(indexVal)) {
                CATCHABLE_OR_RETURN(tryCatchableError(
                    "IndexTypeError", "List index must be a number."));
                break;
            }
            double n = as<Number>(indexVal);
            if (n != std::floor(n)) {
                CATCHABLE_OR_RETURN(tryCatchableError(
                    "IndexNotIntegerError", "List index must be an integer."));
                break;
            }
            auto* list = asObjList(as<Obj*>(listVal));
            int idx = static_cast<int>(n);
            if (idx < 0 || idx >= static_cast<int>(list->elements.size())) {
                CATCHABLE_OR_RETURN(tryCatchableError(
                    "IndexOutOfBoundsError", "List index out of bounds."));
                break;
            }
            list->elements[idx] = val;
            push(val); // assignment is an expression; its value is the assigned
                       // value
            break;
        }
        case Op::SLICE: {
            // Stack (bottom→top): seq, start, end
            Value endVal = peek(0);
            Value startVal = peek(1);
            Value seqVal = peek(2);

            if (!isList(seqVal) && !isString(seqVal)) {
                RAISE_ERROR("Slice requires a List or String.");
                return InterpretResult::RUNTIME_ERROR;
            }
            if (!is<Number>(startVal)) {
                RAISE_ERROR("Slice index must be a number.");
                return InterpretResult::RUNTIME_ERROR;
            }
            double startD = as<Number>(startVal);
            if (startD != std::floor(startD)) {
                RAISE_ERROR("Slice index must be an integer.");
                return InterpretResult::RUNTIME_ERROR;
            }
            if (startD < 0.0) {
                RAISE_ERROR("Slice index must be non-negative.");
                return InterpretResult::RUNTIME_ERROR;
            }
            if (!is<Number>(endVal)) {
                RAISE_ERROR("Slice index must be a number.");
                return InterpretResult::RUNTIME_ERROR;
            }
            double endD = as<Number>(endVal);
            if (endD != std::floor(endD)) {
                RAISE_ERROR("Slice index must be an integer.");
                return InterpretResult::RUNTIME_ERROR;
            }
            if (endD < 0.0) {
                RAISE_ERROR("Slice index must be non-negative.");
                return InterpretResult::RUNTIME_ERROR;
            }

            if (isList(seqVal)) {
                auto* src = asObjList(as<Obj*>(seqVal));
                int n = static_cast<int>(src->elements.size());
                int s = static_cast<int>(std::min(startD, (double)n));
                int e = static_cast<int>(std::min(endD, (double)n));
                int count = (s < e) ? e - s : 0;

                ObjList* result =
                    m_mm.create<ObjList>(VmAllocator<Value>{&m_mm});
                // seqVal is still at peek(2) → src is GC-rooted on the stack
                m_mm.pushTempRoot(result);
                result->elements.resize(count); // may trigger GC
                src =
                    asObjList(as<Obj*>(peek(2))); // re-read after potential GC
                for (int i = 0; i < count; i++) {
                    result->elements[i] = src->elements[s + i];
                }
                m_mm.popTempRoot();
                pop();
                pop();
                pop();
                push(Value{static_cast<Obj*>(result)});
            } else {
                // String — copy chars to local buffer while src is still on
                // stack
                auto* src = asObjString(as<Obj*>(seqVal));
                int n = static_cast<int>(src->chars.size());
                int s = static_cast<int>(std::min(startD, (double)n));
                int e = static_cast<int>(std::min(endD, (double)n));
                std::string substr =
                    (s < e) ? std::string(src->chars.data() + s,
                                          static_cast<size_t>(e - s))
                            : std::string{};
                pop();
                pop();
                pop();
                push(Value{
                    static_cast<Obj*>(m_mm.makeString(std::move(substr)))});
            }
            break;
        }
        case Op::IN: {
            Value seq = pop();
            Value elem = pop();
            if (isList(seq)) {
                auto* list = asObjList(as<Obj*>(seq));
                bool found = false;
                for (const auto& v : list->elements) {
                    if (v == elem) {
                        found = true;
                        break;
                    }
                }
                push(from<bool>(found));
            } else if (isString(seq)) {
                if (!isString(elem)) {
                    RAISE_ERROR(
                        "Left operand of 'in' on a string must be a string.");
                    return InterpretResult::RUNTIME_ERROR;
                }
                auto* haystack = asObjString(as<Obj*>(seq));
                auto* needle = asObjString(as<Obj*>(elem));
                bool found = haystack->chars.find(needle->chars.data(), 0,
                                                  needle->chars.size()) !=
                             LoxString::npos;
                push(from<bool>(found));
            } else if (isMap(seq)) {
                if (is<Number>(elem) && std::isnan(as<Number>(elem))) {
                    CATCHABLE_OR_RETURN(tryCatchableError(
                        "NaNKeyError", "NaN cannot be used as a map key."));
                    break;
                }
                if (is<Obj*>(elem) && as<Obj*>(elem)->type != ObjType::STRING) {
                    CATCHABLE_OR_RETURN(tryCatchableError(
                        "InvalidMapKeyError",
                        "Map keys must be Bool, Number, Nil, or String."));
                    break;
                }
                auto* map = asObjMap(as<Obj*>(seq));
                Value dummy;
                push(from<bool>(map->mapGet(elem, dummy)));
            } else {
                RAISE_ERROR(
                    "Right operand of 'in' must be a list, string, or map.");
                return InterpretResult::RUNTIME_ERROR;
            }
            break;
        }
        case Op::GET_ITER: {
            // peek(0) keeps iterable on stack as GC root during create<>().
            // Mirrors the class-instantiation pattern at vm.cpp:556-558.
            Value iterable = peek(0);
            if (!isList(iterable) && !isString(iterable) && !isMap(iterable)) {
                RAISE_ERROR(
                    "Value is not iterable (expected list, string, or map).");
                return InterpretResult::RUNTIME_ERROR;
            }
            ObjIterator* it = m_mm.create<ObjIterator>(iterable, 0);
            stackTop[-1] = Value{static_cast<Obj*>(it)}; // replace in-place
            break;
        }
        case Op::ITER_HAS_NEXT: {
            Value top = pop();
            // Invariant: value must be an ObjIterator (guaranteed by GET_ITER).
            if (!isIterator(top)) {
                RAISE_ERROR(
                    "BUG: ITER_HAS_NEXT expects an iterator on the stack.");
                return InterpretResult::RUNTIME_ERROR;
            }
            ObjIterator* it = asObjIterator(as<Obj*>(top));
            bool has;
            if (isList(it->collection)) {
                has = it->index <
                      (int)asObjList(as<Obj*>(it->collection))->elements.size();
            } else if (isString(it->collection)) {
                has = it->index <
                      (int)asObjString(as<Obj*>(it->collection))->chars.size();
            } else if (isMap(it->collection)) {
                // Scan forward from current index for the next occupied bucket.
                auto* map = asObjMap(as<Obj*>(it->collection));
                int i = it->index;
                while (i < map->map.capacity() &&
                       map->map.entryAt(i)->state != MapSlot::OCCUPIED) {
                    ++i;
                }
                has = i < map->map.capacity();
            } else {
                RAISE_ERROR(
                    "BUG: ObjIterator::collection has unexpected type.");
                return InterpretResult::RUNTIME_ERROR;
            }
            push(from<bool>(has));
            break;
        }
        case Op::ITER_NEXT: {
            Value top = pop();
            // Invariant: value must be an ObjIterator (guaranteed by GET_ITER).
            if (!isIterator(top)) {
                RAISE_ERROR("BUG: ITER_NEXT expects an iterator on the stack.");
                return InterpretResult::RUNTIME_ERROR;
            }
            ObjIterator* it = asObjIterator(as<Obj*>(top));
            if (isList(it->collection)) {
                push(
                    asObjList(as<Obj*>(it->collection))->elements[it->index++]);
            } else if (isString(it->collection)) {
                char ch =
                    asObjString(as<Obj*>(it->collection))->chars[it->index++];
                // ObjIterator is GC-rooted at iterSlot on VM stack; GC is
                // non-moving. ch is a plain char copied before makeString
                // (which may trigger GC).
                push(Value{static_cast<Obj*>(
                    m_mm.makeString(std::string_view{&ch, 1}))});
            } else if (isMap(it->collection)) {
                // Skip past empty/tombstone buckets to the next occupied one,
                // push its key, then advance the cursor past it.
                auto* map = asObjMap(as<Obj*>(it->collection));
                while (it->index < map->map.capacity() &&
                       map->map.entryAt(it->index)->state !=
                           MapSlot::OCCUPIED) {
                    ++it->index;
                }
                // ITER_HAS_NEXT was true, so an occupied slot must exist.
                push(map->map.entryAt(it->index)->key);
                ++it->index;
            } else {
                RAISE_ERROR(
                    "BUG: ObjIterator::collection has unexpected type.");
                return InterpretResult::RUNTIME_ERROR;
            }
            break;
        }
        case Op::PUSH_HANDLER: {
            uint16_t catchOffset = readShort();
            // catchOffset is relative to the current IP, just like JUMP.
            // ip points to the first byte after the PUSH_HANDLER instruction.
            Chunk::const_iterator catchIp = ip + catchOffset;
            m_handlerStack.push_back(
                HandlerRecord{m_frameCount, stackTop, catchIp});
            break;
        }
        case Op::POP_HANDLER: {
            if (m_handlerStack.empty()) {
                RAISE_ERROR("BUG: POP_HANDLER with empty handler stack.");
                return InterpretResult::RUNTIME_ERROR;
            }
            m_handlerStack.pop_back();
            break;
        }
        case Op::DEFER_RECORD: {
            uint8_t argc = readByte();
            // Pop callee and arguments from stack. Create an ObjDeferredCall
            // object that captures them, and store it on the defer list.
            Value callee = stackTop[-(argc + 1)];
            ObjDeferredCall* deferred =
                m_mm.create<ObjDeferredCall>(callee, VmAllocator<Value>{&m_mm});
            m_mm.pushTempRoot(deferred);
            for (int i = argc - 1; i >= 0; i--) {
                deferred->args.push_back(stackTop[-(i + 1)]);
            }
            m_mm.popTempRoot();
            // Pop arguments and callee from stack.
            stackTop -= argc + 1;
            // Add to defer list for the current frame.
            m_deferLists[m_frameCount - 1].push_back(
                Value{static_cast<Obj*>(deferred)});
            break;
        }
        case Op::RUN_DEFERS: {
            int frameIndex = m_frameCount - 1;
            // The compiler emits RUN_DEFERS only immediately before RETURN,
            // in the same frame, so this is a return leaving this frame —
            // see INVARIANT(handler-stack-frame-scoped) on m_handlerStack's
            // declaration (vm.h). This frame's own stale records must be
            // gone BEFORE its defers run, not only at the RETURN below:
            // otherwise a defer that throws here would still match this
            // frame's own (already-exited) protected region instead of
            // unwinding to the real caller.
            popHandlersOwnedByCurrentFrame();
            // Flush ip into frame->ip first: runPendingDefers may run
            // arbitrary Lox++ code (each deferred call, to completion), and
            // a runtimeError() raised inside it must see this frame's
            // current position, not a stale one (see FrameSync's own
            // comment above for why this matters).
            frame->ip = ip;
            InterpretResult result =
                runPendingDefers(frameIndex, stopAtFrameCount);
            if (result != InterpretResult::OK) {
                return result;
            }
            // Checking against frameIndex here (instead of stopAtFrameCount)
            // was a second instance of R15's bug class: a deferred call's own
            // fault can be caught by a handler ABOVE stopAtFrameCount but AT
            // OR BELOW frameIndex (e.g. an outer try wrapping this frame's
            // own call site) — that catch's frame is a real, still-live
            // frame belonging to THIS SAME run() invocation, and its ip now
            // correctly points at the catch block. Checking frameIndex
            // treated that live catch context as "gone" and returned OK
            // without ever dispatching it (reproduced: defer_throw_outer_
            // catch.lox printed only "middle body" and silently exited 0,
            // never reaching its catch block or "program end"). Only
            // stopAtFrameCount — this run() invocation's own boundary — is
            // the right test: below it, control belongs to a different,
            // less-nested run() invocation (m_frameCount may even be 0);
            // at or above it, any surviving frame is this invocation's own
            // and must be dispatched, whether or not it is still frameIndex.
            if (m_frameCount <= stopAtFrameCount) {
                return InterpretResult::OK;
            }
            FrameSync::loadTop(m_frames, m_frameCount, frame, ip, chunk);
            break;
        }
        case Op::THROW: {
            Value thrownValue = pop();
            // Use the shared unwind implementation (same as runtime faults).
            // Sync frame->ip before calling handleThrow for error reporting.
            frame->ip = ip;
            ThrowOutcome outcome = handleThrow(thrownValue, stopAtFrameCount);
            if (outcome == ThrowOutcome::Uncaught) {
                // No handler found, runtimeError was called, and the stack
                // was reset.
                return InterpretResult::RUNTIME_ERROR;
            }
            if (outcome == ThrowOutcome::HandledStop) {
                // Handled, but by a handler outside THIS run() invocation's
                // own frame range (possibly several reentrant handleThrow
                // calls down — see ThrowOutcome's doc comment in vm.h).
                // m_frameCount may even be 0 here (the resolution ran the
                // rest of the program to completion) — do NOT touch
                // frame/ip/chunk; FrameSync::loadTop would read out of
                // bounds. Hand control back to whichever context started
                // this run() invocation.
                return InterpretResult::OK;
            }
            // HandledContinue: a handler was found and set up, and this
            // run() invocation's own frame context is still live. Reload
            // frame/ip/chunk from the new top and continue dispatch.
            FrameSync::loadTop(m_frames, m_frameCount, frame, ip, chunk);
            break;
        }
        }
    }

#undef BINARY_OP
#undef CATCHABLE_OR_RETURN
#undef RAISE_ERROR
}

bool VM::callNative(ObjNative* native, int argCount) {
    if (native->arity != -1 && argCount != native->arity) {
        runtimeError("Expected %d arguments but got %d.", native->arity,
                     argCount);
        return false;
    }
    m_stdlibCtx.clearError();
    Value result = native->function(argCount, stackTop - argCount);
    if (m_stdlibCtx.nativeError) {
        runtimeError("%s", m_stdlibCtx.nativeErrorMsg.c_str());
        return false;
    }
    stackTop -= argCount + 1; // pop args + callee
    push(result);
    return true;
}

bool VM::callBoundNative(ObjBoundNative* bn, int argCount) {
    ObjNative* fn = bn->native;             // read before the slot changes
    stackTop[-argCount - 1] = bn->receiver; // natives read args[-1]
    return callNative(fn, argCount);
}

bool VM::bindMethod(ObjClass* klass, ObjString* name) {
    Value method;
    if (!klass->methods.get(name, method)) {
        runtimeError("Undefined property '%s'.", name->chars.c_str());
        return false;
    }
    ObjBoundMethod* bound =
        m_mm.create<ObjBoundMethod>(peek(0), asObjClosure(as<Obj*>(method)));
    pop(); // instance
    push(Value{static_cast<Obj*>(bound)});
    return true;
}

void VM::defineNatives() {
    StdlibRegistrar reg(m_mm, m_globals);
    registerGlobals(reg);
    m_fileClass = registerFileAPI(reg);
    m_mapClass = registerMapAPI(reg);
    m_errorClass = registerErrorAPI(reg);
    registerMath(reg);
    registerOSAPI(reg, m_mapClass);
    registerReflectAPI(reg);
}

void VM::runtimeError(const char* format, ...) {
    va_list args;
    va_start(args, format);
    // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputs("\n", stderr);

    // Print a stack trace (innermost frame first).
    for (int i = m_frameCount - 1; i >= 0; i--) {
        const CallFrame& frame = m_frames[i];
        ObjFunction* fn = frame.closure->function;
        const Chunk& chunk = fn->chunk;
        auto offset = static_cast<int>(frame.ip - chunk.cbegin()) - 1;
        int line = chunk.getLine(offset);
        std::fprintf(stderr, "[line %d] in ", line);
        if (fn->name == nullptr) {
            std::fprintf(stderr, "script\n");
        } else {
            std::fprintf(stderr, "%s()\n", fn->name->chars.c_str());
        }
    }

    resetStack();
}

void VM::markRoots() {
    for (Value* slot = stack; slot < stackTop; ++slot) {
        m_mm.markValue(*slot);
    }
    for (int i = 0; i < m_frameCount; ++i) {
        m_mm.markObject(m_frames[i].closure);
        // Mark pending deferred calls in this frame.
        for (const Value& defer : m_deferLists[i]) {
            m_mm.markValue(defer);
        }
    }
    for (ObjUpvalue* uv = m_openUpvalues; uv != nullptr; uv = uv->next) {
        m_mm.markObject(uv);
    }
    m_globals.forEach([this](ObjString* key, Value val) {
        m_mm.markObject(key);
        m_mm.markValue(val);
    });
    if (m_fileClass) {
        m_mm.markObject(m_fileClass);
    }
    if (m_mapClass) {
        m_mm.markObject(m_mapClass);
    }
    if (m_errorClass) {
        m_mm.markObject(m_errorClass);
    }
}

void VM::resetStack() {
    stackTop = stack;
    m_frameCount = 0;
    m_stackOverflow = false;
    m_handlerStack.clear();
    for (auto& deferList : m_deferLists) {
        deferList.clear();
    }
}

void VM::push(Value value) {
    // Same threshold, STACK_MAX, whether or not a handler is active (see
    // STACK_OVERFLOW_STACK_RESERVE's own comment in vm.h): an open try/catch
    // must never change how deep a program that does not overflow can go.
    // While unwinding a StackOverflowError (m_unwindingStackOverflow), the
    // ceiling moves out to STACK_MAX + STACK_OVERFLOW_STACK_RESERVE — the
    // physical capacity `stack` actually has — so a deferred call drained
    // during that unwind can use the reserve; past that, it stays fatal.
    std::ptrdiff_t hardCeiling =
        STACK_MAX +
        (m_unwindingStackOverflow ? STACK_OVERFLOW_STACK_RESERVE : 0);
    if (stackTop == stack + hardCeiling) {
        m_stackOverflow = true;
        return;
    }
    *stackTop++ = value;
}

Value VM::pop() { return *--stackTop; }

Value VM::peek(int distance) { return stackTop[-1 - distance]; }
